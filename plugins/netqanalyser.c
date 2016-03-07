/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "netqanalyser.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

//#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
//#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
//#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
//#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define THIS_READLOCK(this)
#define THIS_READUNLOCK(this)
#define THIS_WRITELOCK(this)
#define THIS_WRITEUNLOCK(this)

#define AGGRESSIVITY 0.1f

GST_DEBUG_CATEGORY_STATIC (netqueue_analyser_debug_category);
#define GST_CAT_DEFAULT conetqueue_analyser_debug_category

G_DEFINE_TYPE (NetQueueAnalyser, netqueue_analyser, G_TYPE_OBJECT);

typedef struct _CorrBlock CorrBlock;

struct _CorrBlock{
  guint           id,N;
  gint64          Iu0,Iu1,Id1,Id2,Id3,G01,M0,M1,G_[16],M_[16];
  gint            index;
  gdouble         g,g1,g_next,g_dev,distortion_th;
  gboolean        distorted;
  CorrBlock*     next;
};
#define CORR_LOG_ON

static void _execute_corrblocks(NetQueueAnalyserPrivate *this, CorrBlock *blocks, guint blocks_length);
static void _execute_corrblock(CorrBlock* this);

typedef struct _NetQueueAnalyserPrivate{
  CorrBlock           cblocks[6];
  guint32             cblocks_counter;
}NetQueueAnalyserPrivate;


#define _priv(this) ((NetQueueAnalyserPrivate*) this->priv)
#define _now(this) (gst_clock_get_time(this->sysclock))
//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------
static void _log_abbrevations(NetQueueAnalyser *this, FILE *file);

static void netqueue_analyser_finalize (GObject * object);

static void _qdeanalyzer_evaluation(NetQueueAnalyser *this, NetQueueAnalyserResult *result);

void
netqueue_analyser_class_init (NetQueueAnalyserClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = netqueue_analyser_finalize;

  GST_DEBUG_CATEGORY_INIT (netqueue_analyser_debug_category, "netqueue_analyser", 0,
      "NetQueueAnalyser");

}

void
netqueue_analyser_finalize (GObject * object)
{
  NetQueueAnalyser *this;
  this = NETQANALYSER(object);

  g_free(this->priv);
  g_object_unref(this->sysclock);
}

void
netqueue_analyser_init (NetQueueAnalyser * this)
{
  g_rw_lock_init (&this->rwmutex);
  this->sysclock = gst_system_clock_obtain();
  this->priv = g_malloc0(sizeof(NetQueueAnalyserPrivate));
}

NetQueueAnalyser *make_netqueue_analyser(void)
{
  NetQueueAnalyser *this;
  this = g_object_new (NETQANALYSER_TYPE, NULL);
  THIS_WRITELOCK (this);

  _priv(this)->cblocks[0].next = &_priv(this)->cblocks[1];
  _priv(this)->cblocks[1].next = &_priv(this)->cblocks[2];
  _priv(this)->cblocks[2].next = &_priv(this)->cblocks[3];
  _priv(this)->cblocks[3].next = &_priv(this)->cblocks[4];
  _priv(this)->cblocks[0].id   = 0;
  _priv(this)->cblocks[1].id   = 1;
  _priv(this)->cblocks[2].id   = 2;
  _priv(this)->cblocks[3].id   = 3;
  _priv(this)->cblocks[4].id   = 4;
  _priv(this)->cblocks[0].N    = 4;
  _priv(this)->cblocks[1].N    = 4;
  _priv(this)->cblocks[2].N    = 4;
  _priv(this)->cblocks[3].N    = 4;
  _priv(this)->cblocks[4].N    = 4;
  _priv(this)->cblocks[0].distortion_th   = 0.025;
  _priv(this)->cblocks[1].distortion_th   = 0.05;
  _priv(this)->cblocks[2].distortion_th   = 0.1;
  _priv(this)->cblocks[3].distortion_th   = 1.;
  _priv(this)->cblocks[4].distortion_th   = 1.;
  _priv(this)->cblocks_counter = 1;

  THIS_WRITEUNLOCK (this);
  return this;
}


void netqueue_analyser_reset(NetQueueAnalyser *this)
{
  THIS_WRITELOCK (this);
  THIS_WRITEUNLOCK (this);
}

void netqueue_analyser_reset_stability(NetQueueAnalyser *this)
{
  this->last_stable = 0;
}

void netqueue_analyser_do(NetQueueAnalyser *this,
                          GstMPRTCPReportSummary *summary,
                          NetQueueAnalyserResult *result)
{
  gint i;
  gdouble tr_ratio;
  gboolean congestion;

  if(!summary->XR_OWD_RLE.processed){
    g_warning("NetQueue Analyser can not work without OWD RLE");
    return;
  }

  memset(result, 0, sizeof(NetQueueAnalyserResult));

  if(summary->XR_RFC7097.processed){
    result->pierced |= 0 < summary->XR_RFC7097.length;
  }

  if(summary->XR_RFC7243.processed){
    result->pierced |= 0 < summary->XR_RFC7243.discarded_bytes;
  }

  if(summary->XR_RFC3611.processed){
    result->congested |= 0 < summary->XR_RFC3611.length;
  }

  if(summary->RR.processed){
    result->congested |= 0. < summary->RR.lost_rate;
  }

  congestion = 0 < summary->XR_OWD_RLE.length;

  for(i=0; i<summary->XR_OWD_RLE.length; ++i){
    GstClockTime delay;
    delay = summary->XR_OWD_RLE.values[i];
    if(!delay){
        congestion = FALSE;
        continue;
    }
    congestion &= delay == GST_SECOND;

    _priv(this)->cblocks[0].Iu0 = GST_TIME_AS_USECONDS(delay) / 50.;
    _execute_corrblocks(_priv(this), _priv(this)->cblocks, 4);
    _execute_corrblocks(_priv(this), _priv(this)->cblocks, 4);
    _priv(this)->cblocks[0].Id1 = GST_TIME_AS_USECONDS(delay) / 50.;
  }

  result->congested |= congestion;
  _qdeanalyzer_evaluation(this, result);

}

//----------------------------------------------------------------------------------------
//                  Queue Delay Analyzation
//----------------------------------------------------------------------------------------

void _qdeanalyzer_evaluation(NetQueueAnalyser *this, NetQueueAnalyserResult *result)
{

  result->pierced   |= _priv(this)->cblocks[0].distorted || _priv(this)->cblocks[2].g < -.01;
  result->distorted |= result->pierced   && _priv(this)->cblocks[1].distorted;
  result->congested |= result->distorted && _priv(this)->cblocks[2].distorted;

  _priv(this)->cblocks[0].distorted = FALSE;
  _priv(this)->cblocks[1].distorted = FALSE;
  _priv(this)->cblocks[2].distorted = FALSE;

  if(result->pierced || result->distorted || result->congested){
    result->stability = 0;
    this->last_stable = 0;
  }else if(this->last_stable == 0){
    this->last_stable = _now(this);
    result->stability = 0;
  }else{
    result->stability = GST_TIME_AS_SECONDS(_now(this) - this->last_stable);
  }
}


void _execute_corrblocks(NetQueueAnalyserPrivate *this, CorrBlock *blocks, guint blocks_length)
{
  guint32 X = (this->cblocks_counter ^ (this->cblocks_counter-1))+1;
  switch(X){
    case 2:
      _execute_corrblock(blocks);
    break;
    case 4:
          _execute_corrblock(blocks + 1);
        break;
    case 8:
          _execute_corrblock(blocks + 2);
        break;
    case 16:
          _execute_corrblock(blocks + 3);
        break;
    case 32:
          _execute_corrblock(blocks + 4);
        break;
    case 64:
          _execute_corrblock(blocks + 5);
        break;
    default:
//      g_print("not execute: %u\n", X);
      break;
  }

  ++this->cblocks_counter;
}

void _execute_corrblock(CorrBlock* this)
{
  this->M1   = this->M0;
  this->M0  -= this->M_[this->index];
  this->G01 -= this->G_[this->index];
  this->M0  += this->M_[this->index] = this->Iu0;
  this->G01 += this->G_[this->index] = this->Iu0 * this->Id1;
  if(this->M0 && this->M1){
    this->g1 = this->g;
    this->g  = this->G01 * (this->N);
    this->g /= this->M0 * this->M1;
    this->g -= 1.;
  }
  if(++this->index == this->N) {
    this->index = 0;
  }

  if(this->next){
    CorrBlock *next = this->next;
    next->Iu0 = this->Iu0 + this->Iu1;
    next->Id1 = this->Id2 + this->Id3;
  }

  this->Iu1  = this->Iu0;
  this->Id3  = this->Id2;
  this->Id2  = this->Id1;

  if(this->M0 && this->M1){
    this->g_next = this->g + this->g1;
    this->g_dev += ((this->g - this->g1) - this->g_dev) / (gdouble)(2 * this->N);
    if(this->g_dev < 0.) this->g_dev *=-1.;
  }
  if(this->distortion_th < this->g){
    this->distorted = TRUE;
  }
}

#undef CORR_LOG_ON
#undef _swap_sitems
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef THIS_READLOCK
#undef THIS_READUNLOCK
