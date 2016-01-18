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
#include "subratectrler.h"
#include "gstmprtcpbuffer.h"
#include <math.h>
#include <string.h>
#include "bintree.h"

//#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
//#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
//#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
//#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define THIS_READLOCK(this)
#define THIS_READUNLOCK(this)
#define THIS_WRITELOCK(this)
#define THIS_WRITEUNLOCK(this)

GST_DEBUG_CATEGORY_STATIC (subratectrler_debug_category);
#define GST_CAT_DEFAULT subratectrler_debug_category

G_DEFINE_TYPE (SubflowRateController, subratectrler, G_TYPE_OBJECT);

#define MOMENTS_LENGTH 8


// Min OWD target. Default value: 0.1 -> 100ms
#define OWD_TARGET_LO 100 * GST_MSECOND
//Max OWD target. Default value: 0.4s -> 400ms
#define OWD_TARGET_HI 400 * GST_MSECOND
//Headroom for limitation of CWND. Default value: 1.1
#define MAX_BYTES_IN_FLIGHT_HEAD_ROOM 1.1
//Gain factor for congestion window adjustment. Default value:  1.0
#define GAIN_UP 1.0
//Gain factor for congestion window adjustment. Default value:  1.0
#define GAIN_DOWN 1.0
//Initial cwnd regarding to minimal bitrate
#define INIT_CWND 5000
// Max video rampup speed in bps/s (bits per second increase per second)
#define RAMP_UP_SPEED 200000.0f // bps/s
//CWND scale factor due to loss event. Default value: 0.6
#define BETA 0.6
// Target rate scale factor due to loss event. Default value: 0.8
#define BETA_R 0.8
//Additional slack [%]  to the congestion window. Default value: 10%
#define BYTES_IN_FLIGHT_SLACK .1
//Interval between video bitrate adjustments. Default value: 0.2s ->200ms
#define RATE_ADJUST_INTERVAL 200 * GST_MSECOND /* ms */
//Video coder frame period [s]
#define FRAME_PERIOD 0
//Min target_bitrate [bps]
#define TARGET_BITRATE_MIN 500000
//Max target_bitrate [bps]
#define TARGET_BITRATE_MAX 0
//Timespan [s] from lowest to highest bitrate. Default value: 10s->10000ms
#define RAMP_UP_TIME 10000
//Guard factor against early congestion onset.
//A higher value gives less jitter possibly at the
//expense of a lower video bitrate. Default value: 0.0..0.2
#define PRE_CONGESTION_GUARD 0.0
//Guard factor against RTP queue buildup. Default value: 0.0..2.0
#define TX_QUEUE_SIZE_FACTOR 1.0


typedef struct{
  gdouble avg;
  gdouble x1;
  gdouble a0,a1;
}RData;

typedef struct _Moment Moment;

typedef enum{
  STATE_OVERUSED      = -1,
  STATE_STABLE         =  0,
  STATE_MONITORED      =  1,
}State;

typedef enum{
  STAGE_RESTRICTED      = 4,
  STAGE_RELEASED          = 5,
}Stage;

//ToDO Apply it
typedef enum{
  BITRATE_UP           =  1,
  BITRATE_STAY         =  0,
  BITRATE_DOWN         = -1,
}BitrateAim;


struct _Moment{
  GstClockTime    time;
  guint64         recent_delay;
  guint64         RTT;
  guint64         ltt_delays_th;
  guint64         ltt_delays_target;
  guint32         jitter;
  guint32         lost;
  guint32         discard;
  gint32          receiver_rate;
  gint32          sender_rate;
  gint32          goodput;
  guint32         bytes_newly_acked;
  guint32         bytes_in_flight_ack;
  gint64          bytes_in_flight_ested;
  gdouble         BiF_off;
  guint32         bytes_in_queue;
  guint64         max_bytes_in_flight;

  gboolean        recent_discard;
  gboolean        recent_lost;
  gboolean        path_is_lossy;
  gboolean        path_is_slow;

  //derivatives
  guint32         receiver_rate_std;
  gdouble         discard_rate;
  gdouble         corrh_owd;
//  gdouble         rate_corr;
  gdouble         del_corr;
//  gdouble         BiF_off_target;
  gboolean        can_bitrate_increase;
  gdouble         off_target;
  BitrateAim      bitrate_aim;
  //OWD trend indicates incipient congestion. Initial value: 0.0
  gdouble         owd_trend;
  gdouble         BiF_corr;
  gint32          delta_rate;
//  gint32          delta_cwnd;

  //application
  State           state;
  gboolean        controlled;

};



//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void subratectrler_finalize (GObject * object);


//static const gdouble ST_ = 1.1; //Stable treshold
//static const gdouble OT_ = 2.;  //Overused treshold
//static const gdouble DT_ = 1.5; //Down Treshold


//--------------------MEASUREMENTS-----------------------

#define _actual_rate(this) (MAX(_mt0(this)->sender_rate, _mt0(this)->receiver_rate))
#define _ramp_up_speed(this) (MIN(RAMP_UP_SPEED, this->target_bitrate))
#define _now(this) (gst_clock_get_time(this->sysclock))
#define _get_moment(this, n) ((Moment*)(this->moments + n * sizeof(Moment)))
#define _pmtn(index) (index == MOMENTS_LENGTH ? MOMENTS_LENGTH - 1 : index == 0 ? MOMENTS_LENGTH - 1 : index - 1)
#define _mt0(this) _get_moment(this, this->moments_index)
#define _mt1(this) _get_moment(this, _pmtn(this->moments_index))
#define _mt2(this) _get_moment(this, _pmtn(_pmtn(this->moments_index)))

//FixME: apply trend calculation instead of recent discard
#define _rdiscard(this) (0 && _mt0(this)->recent_discard)
//FixME: apply trend calculation instead of recent discard
#define _discard(this) (0 && _mt0(this)->discard_rate > .1)
#define _lost(this) (_mt0(this)->path_is_lossy && _mt0(this)->lost)
#define _rlost(this) (_mt0(this)->path_is_lossy && _mt0(this)->recent_lost)
#define _corrH(this) (_mt0(this)->corrh_owd)
#define _state_t1(this) _mt1(this)->state
#define _state(this) _mt0(this)->state
#define _otrend(this) _mt0(this)->owd_trend
#define _GP(this) _mt0(this)->goodput
#define _BiFCorr(this) _mt0(this)->BiF_corr
#define _GP_t1(this) _mt1(this)->goodput
#define _RR(this) _mt0(this)->receiver_rate
#define _RR_t1(this) _mt1(this)->receiver_rate
#define _SR(this) _mt0(this)->sender_rate
#define _TR(this) this->target_bitrate
#define _rateCorr(this) (gdouble)_SR(this) / (gdouble)_RR(this)
#define _maxCorr(this) (gdouble)_RR(this) / (gdouble)this->max_target_point
#define _sCorr(this)  (gdouble) _SR(this) / (gdouble) _TR(this)
#define _tCorr(this) (gdouble)_TR(this) / (gdouble)this->max_target_point
#define _delcorr(this) _mt0(this)->del_corr
#define _delcorr_t1(this) _mt1(this)->del_corr
#define _dCorr(this) (_delcorr(this)) / (_delcorr_t1(this))
#define _add_target_point(this, target) numstracker_add(this->target_points, target)

#define _set_adjustment_aim(this, aim) _mt0(this)->bitrate_aim = aim
#define _bitrate_aim(this) _mt0(this)->bitrate_aim


static Moment*
_m_step(
    SubflowRateController *this);

static void
_update_bitrate(
    SubflowRateController *this);

static void
_restricted_stage(
    SubflowRateController *this);
static void
_released_stage(
    SubflowRateController *this);

static void _change_overused_stage_to(
    SubflowRateController *this,
    Stage target);

static void
_overused_state(
    SubflowRateController *this);

static void
_stable_state(
    SubflowRateController *this);

static void
_monitored_state(
    SubflowRateController *this);

static void
_reduce(
    SubflowRateController *this,
    gboolean disable_controlling);

static void
_undershoot(
    SubflowRateController *this,
    gboolean disable_controlling);

static void
_transit_to(
    SubflowRateController *this,
    State target);

static void
_set_pacing_bitrate(
    SubflowRateController *this,
    guint32 target_bitrate,
    gboolean enable_pacing);

#define _enable_pacing(this) _set_path_pacing(this, TRUE)
#define _disable_pacing(this) _set_path_pacing(this, FALSE)
#define _pacing_enabled(this) this->path_is_paced

static void
_set_path_pacing(
    SubflowRateController *this,
    gboolean enable_pacing);

static gdouble
_adjust_bitrate(
    SubflowRateController *this);

static void
_disable_controlling(
    SubflowRateController *this);

#define _disable_monitoring(this) _set_monitoring_interval(this, 0)

static gboolean
_monitoring_is_allowed(
    SubflowRateController *this);

static void
_reset_monitoring(
    SubflowRateController *this);

static void
_setup_monitoring(
    SubflowRateController *this);

static void
_set_monitoring_interval(
    SubflowRateController *this,
    guint interval);

static guint
_calculate_monitoring_interval(
    SubflowRateController *this,
    guint32 desired_bitrate);

static void
_set_owd_trend(
    SubflowRateController *this);

static gdouble
_get_off_target(
    SubflowRateController *this);

//static gboolean
//_cwnd_can_increase(SubflowRateController *this);

static void
_ltt_delays_th_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata);

static void
_change_target_bitrate(SubflowRateController *this, gint32 new_target);

static void
_ltt_delays_target_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata);

static void
_target_points_max_pipe(gpointer data, guint64 value);

static void
_target_points_min_pipe(gpointer data, guint64 value);

static void
_bights_in_flight_max_pipe(gpointer data, guint64 value);


static void
_print_mupdate_state(SubflowRateController *this);

static void
_print_undershoot(SubflowRateController *this,
                       gint32 picked_GP,
                       gint32 drate);

//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
subratectrler_class_init (SubflowRateControllerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = subratectrler_finalize;

  GST_DEBUG_CATEGORY_INIT (subratectrler_debug_category, "subratectrler", 0,
      "MpRTP Manual Sending Controller");

}

void
subratectrler_finalize (GObject * object)
{
  SubflowRateController *this;
  this = SUBRATECTRLER(object);
  g_object_unref(this->sysclock);
}

void
subratectrler_init (SubflowRateController * this)
{
  this->moments = g_malloc0(sizeof(Moment) * MOMENTS_LENGTH);
  this->sysclock = gst_system_clock_obtain();
  g_rw_lock_init (&this->rwmutex);
  this->ltt_delays_th = make_percentiletracker(100, 80);
  percentiletracker_set_treshold(this->ltt_delays_th, GST_SECOND * 60);
  percentiletracker_set_stats_pipe(this->ltt_delays_th,
                                   _ltt_delays_th_stats_pipe, this);
  this->ltt_delays_target = make_percentiletracker(100, 50);
  percentiletracker_set_treshold(this->ltt_delays_target, GST_SECOND * 60);
  percentiletracker_set_stats_pipe(this->ltt_delays_target,
                                   _ltt_delays_target_stats_pipe, this);

  this->owd_fraction_hist = make_floatnumstracker(20, 5 * GST_SECOND);

  this->bytes_in_flight_history = make_numstracker(16, 10 * GST_SECOND);
  numstracker_add_plugin(this->bytes_in_flight_history,
                         (NumsTrackerPlugin*) make_numstracker_minmax_plugin(_bights_in_flight_max_pipe, this, NULL, NULL));

  this->target_points = make_numstracker(16, 5 * GST_SECOND);
  numstracker_add_plugin(this->target_points,
                         (NumsTrackerPlugin*) make_numstracker_minmax_plugin(_target_points_max_pipe, this, _target_points_min_pipe, this));

}


SubflowRateController *make_subratectrler(void)
{
  SubflowRateController *result;
  result = g_object_new (SUBRATECTRLER_TYPE, NULL);
  return result;
}

void subratectrler_set(SubflowRateController *this,
                         MPRTPSPath *path,
                         guint32 sending_target)
{
  THIS_WRITELOCK(this);
  percentiletracker_reset(this->ltt_delays_th);
  percentiletracker_reset(this->ltt_delays_target);
  this->path = g_object_ref(path);
  memset(this->moments, 0, sizeof(Moment) * MOMENTS_LENGTH);

  this->id = mprtps_path_get_id(this->path);
  this->monitoring_interval = 3;
  this->owd_fraction_avg = 0.;
  this->BiF_ested_avg = 0.;
  this->mss = 1400;
  this->cwnd_min = 3 * this->mss;
  this->cwnd_i = 1;
  this->pacing_bitrate = INIT_CWND;
  this->target_bitrate = sending_target * 8;
  numstracker_add(this->target_points, this->target_bitrate);
  this->s_rtt = 0.;
  this->min_rate = TARGET_BITRATE_MIN;
  this->max_rate = TARGET_BITRATE_MAX;

  _transit_to(this, STATE_STABLE);
  THIS_WRITEUNLOCK(this);
}

void subratectrler_unset(SubflowRateController *this)
{
  THIS_WRITELOCK(this);
  g_object_unref(this->path);
  this->path = NULL;
  THIS_WRITEUNLOCK(this);
}

void subratectrler_extract_stats(SubflowRateController *this,
                                  guint64 *median_delay,
                                  gint32  *sender_rate,
                                  gdouble *target_rate,
                                  gdouble *goodput,
                                  gdouble *next_target)
{
  if(median_delay)
    *median_delay = percentiletracker_get_stats(this->ltt_delays_th, NULL, NULL, NULL);
  if(sender_rate)
    *sender_rate = _mt0(this)->sender_rate;
  if(target_rate)
    *target_rate = this->target_bitrate / 8;
  if(goodput)
    *goodput = _mt0(this)->goodput;
  if(next_target)
    *next_target = 0;
}

void subratectrler_time_update(
                         SubflowRateController *this,
                         gint32 *target_bitrate,
                         gint32 *extra_bitrate,
                         UtilizationSubflowReport *rep)
{


  _mt0(this)->sender_rate         = mprtps_path_get_sender_rate(this->path) * 8;
  _mt0(this)->bytes_in_queue      = mprtps_path_get_bytes_in_queue(this->path);

  if(this->moments_num == 0){
      this->bytes_in_queue_avg = _mt0(this)->bytes_in_queue;
  }else{
      this->bytes_in_queue_avg *= .5;
      this->bytes_in_queue_avg += _mt0(this)->bytes_in_queue * .5;
  }
  this->s_SR = .8 * this->s_SR + .2 * _mt0(this)->sender_rate;

  mprtps_path_get_bytes_in_flight(this->path, NULL, &_mt0(this)->bytes_in_flight_ested);
  if(0 < _mt0(this)->bytes_in_flight_ested){
      gdouble off;
      this->BiF_ested_avg = .8* this->BiF_ested_avg + .2* (gdouble)_mt0(this)->bytes_in_flight_ested;
      off = (this->BiF_ested_avg - this->BiF_acked_avg) / this->BiF_ested_avg;
      _mt0(this)->BiF_off = off;
      this->BiF_off_avg = .1 * off + .9 * this->BiF_off_avg;

//    _mt0(this)->BiF_off_target =    MAX(-1., MIN(1., this->BiF_off_ested_avg));
  }

  if(!numstracker_get_num(this->target_points)){
    _add_target_point(this, _TR(this));
  }

  _update_bitrate(this);

  if(rep){
    rep->lost_bytes = _mt0(this)->lost;
    rep->discarded_bytes = _mt0(this)->discard;
    rep->owd = _mt0(this)->recent_delay;
    rep->max_rate = this->max_rate;
  }
  if(target_bitrate)
    *target_bitrate = this->target_bitrate;
  if(extra_bitrate)
    *extra_bitrate = this->monitored_bitrate;

  return;
}

void subratectrler_measurement_update(
                         SubflowRateController *this,
                         RRMeasurement * measurement)
{
  gint i;
//  g_print_rrmeasurement(measurement);
  if(measurement->goodput <= 0.) goto done;
  _m_step(this);

  _mt0(this)->time                = measurement->time;
  _mt0(this)->recent_delay        = measurement->recent_delay;
  _mt0(this)->RTT                 = measurement->RTT;
  _mt0(this)->discard             = measurement->late_discarded_bytes;
  _mt0(this)->lost                = measurement->lost;
  _mt0(this)->recent_lost         = measurement->recent_lost;
  _mt0(this)->recent_discard      = measurement->recent_discard;
  _mt0(this)->path_is_lossy       = !mprtps_path_is_non_lossy(this->path);
  _mt0(this)->path_is_slow        = !mprtps_path_is_not_slow(this->path);
  _mt0(this)->goodput             = measurement->goodput * 8;
  _mt0(this)->receiver_rate       = measurement->receiver_rate * 8;
  _mt0(this)->jitter              = measurement->jitter;
  _mt0(this)->bytes_newly_acked   = measurement->expected_payload_bytes;
  _mt0(this)->bytes_in_flight_ack = measurement->bytes_in_flight_acked;

  _mt0(this)->BiF_off             = _mt1(this)->BiF_off;
  _mt0(this)->bitrate_aim         = _mt1(this)->bitrate_aim;
  _mt0(this)->bytes_in_flight_ested = _mt1(this)->bytes_in_flight_ested;
  _mt0(this)->sender_rate         = _mt1(this)->sender_rate;
  _mt0(this)->bytes_in_queue      = _mt1(this)->bytes_in_queue;
  _mt0(this)->max_bytes_in_flight = _mt1(this)->max_bytes_in_flight;
  _mt0(this)->ltt_delays_th       = _mt1(this)->ltt_delays_th;
  _mt0(this)->ltt_delays_target   = _mt1(this)->ltt_delays_target;
  _mt0(this)->state               = _mt1(this)->state;

  _mt0(this)->discard_rate        = 1. - measurement->goodput / measurement->receiver_rate;
//  _mt0(this)->max_corr            = MIN((gdouble) _RR(this) / (gdouble)_SR(this), (gdouble) _RR_t1(this) / (gdouble) _SR(this));
//  _mt0(this)->max_corr            = (gdouble)(_RR(this) + _RR_t1(this)) / (this->s_SR * 2.);
//  _mt0(this)->rate_corr            = (gdouble)(_RR(this) ) / (this->s_SR);

  if(1 < this->moments_num)
    this->s_rtt = measurement->RTT * .125 + this->s_rtt * .875;
  else
    this->s_rtt = measurement->RTT;

  if(_mt1(this)->bytes_in_flight_ack){
    _mt0(this)->BiF_corr = (gdouble) _mt0(this)->bytes_in_flight_ack / (gdouble)_mt1(this)->bytes_in_flight_ack;
  }else{
    _mt0(this)->BiF_corr = 1.;
  }

  if(numstracker_get_num(this->bytes_in_flight_history) < 1){
      _mt0(this)->max_bytes_in_flight = measurement->bytes_in_flight_acked;
  }

  percentiletracker_obsolate(this->ltt_delays_target);
  if(!percentiletracker_get_num(this->ltt_delays_target)){
      percentiletracker_add(this->ltt_delays_target, OWD_TARGET_LO);
  }

  percentiletracker_obsolate(this->ltt_delays_th);
  if(!percentiletracker_get_num(this->ltt_delays_th)){
      percentiletracker_add(this->ltt_delays_th, OWD_TARGET_HI);
  }

  this->BiF_acked_avg = .2* this->BiF_acked_avg + .8* (gdouble)_mt0(this)->bytes_in_flight_ack;

  for(i=0; i<measurement->rle_delays.length; ++i){
    gdouble owd_fraction, delay;
    delay = measurement->rle_delays.values[i];
    owd_fraction = delay/(gdouble)_mt0(this)->ltt_delays_target;
    floatnumstracker_add(this->owd_fraction_hist, owd_fraction);
    this->owd_fraction_avg = .9* this->owd_fraction_avg + .1* owd_fraction;
    this->delay_t3 = this->delay_t2;
    this->delay_t2 = this->delay_t1;
    this->delay_t1 = this->delay_t0;
    this->delay_t0 = delay;
  }
  _mt0(this)->del_corr = (.125 * this->delay_t2 + .375 * this->delay_t1 + .5 * this->delay_t0) / ((gdouble)_mt0(this)->ltt_delays_target);

  if(1. < _delcorr_t1(this) && _delcorr(this) < 1.){
    this->last_congestion_point = _RR(this);
  }

  if(!measurement->rle_delays.length){
    gdouble owd_fraction;
    owd_fraction = (gdouble)measurement->recent_delay/(gdouble)_mt0(this)->ltt_delays_target;
    this->owd_fraction_avg = .9* this->owd_fraction_avg + .1* owd_fraction;
    floatnumstracker_add(this->owd_fraction_hist, owd_fraction);
  }

  _set_owd_trend(this);
  mprtps_path_set_delay(this->path, _mt0(this)->ltt_delays_target);

  if(!_mt0(this)->ltt_delays_th) _mt0(this)->ltt_delays_th = OWD_TARGET_HI;
  _mt0(this)->corrh_owd = (gdouble)_mt0(this)->recent_delay / (gdouble)_mt0(this)->ltt_delays_th;
  _mt0(this)->can_bitrate_increase = _mt0(this)->state != STATE_OVERUSED;
  _mt0(this)->can_bitrate_increase &= this->target_bitrate < _mt0(this)->receiver_rate * 1.1;

  _mt0(this)->off_target = _get_off_target(this);

  if(0 < this->disable_controlling && this->disable_controlling < _now(this)){
    this->disable_controlling = 0;
  }

  if(this->disable_controlling == 0LU){
    this->controller(this);
    _mt0(this)->controlled = TRUE;
  }
  _print_mupdate_state(this);

  if(_mt0(this)->state != STATE_OVERUSED){
    percentiletracker_add(this->ltt_delays_th, measurement->recent_delay);
    percentiletracker_add(this->ltt_delays_target, measurement->recent_delay);
    numstracker_add(this->bytes_in_flight_history, measurement->bytes_in_flight_acked);
    this->packet_obsolation_treshold = OWD_TARGET_HI - _mt0(this)->ltt_delays_target;
    for(i=0; i<measurement->rle_delays.length; ++i){
      guint64 delay;
      delay = measurement->rle_delays.values[i];
      if(!measurement->rle_delays.values[i]) continue;
      //Percentiletracker has a tiny problem with similar value handling.
      //That is why we add some random ns value, in order to
      //get a reasonable median target value
      delay+= g_random_int_range(0, 1000);
      percentiletracker_add(this->ltt_delays_th, delay);
      percentiletracker_add(this->ltt_delays_target, delay);
    }
    _add_target_point(this, _RR(this));
    _add_target_point(this, _RR(this) * 1.025);
//    _add_target_point(this, _RR(this) * .975);
  }
done:
  return;
}

gint32 subratectrler_get_target_bitrate(SubflowRateController *this)
{
  return this->target_bitrate;
}

void subratectrler_add_extra_rate(SubflowRateController *this,
                                  gint32 extra_rate)
{

}

Moment* _m_step(SubflowRateController *this)
{
  if(++this->moments_index == MOMENTS_LENGTH){
    this->moments_index = 0;
  }
  ++this->moments_num;
  memset(_mt0(this), 0, sizeof(Moment));
  return _mt0(this);
}

void
_update_bitrate(SubflowRateController *this)
{
  gdouble queue_ratio;
  if (_now(this) - RATE_ADJUST_INTERVAL < this->last_target_bitrate_adjust) {
    goto exit;
  }

  queue_ratio = (gdouble)(this->bytes_in_queue_avg * 8) / (gdouble) this->target_bitrate;
  if(.5 < queue_ratio){
    if(this->last_queue_clear < _now(this) - 5 * GST_SECOND){
      mprtps_path_clear_queue(this->path);
      this->last_queue_clear = _now(this);
    }else if(this->last_skip_time < _now(this) - GST_SECOND){
      mprtps_path_set_skip_duration(this->path, 500 * GST_MSECOND);
      this->last_skip_time = _now(this);
    }
  }

  _change_target_bitrate(this, _adjust_bitrate(this));

//done:
  this->last_target_bitrate_adjust = _now(this);
exit:
  return;
}


void
_restricted_stage(
    SubflowRateController *this)
{
  //check weather delay reached the inflection point
  if(2. < _corrH(this)){
    if(1. <= _dCorr(this)){
      _reduce(this, TRUE);
      DISABLE_LINE _undershoot(this, TRUE);
    }
    goto done;
  }

  //Check weather sending rate correlate to the target rate
  if(1.1 < _sCorr(this)){
    goto done;
  }

  //Check weather sender and receiver rates are correlated
  if(1.1 < _rateCorr(this)){
    goto done;
  }

  //increase pacing bitrate
  _add_target_point(this, _RR(this));
  _set_pacing_bitrate(this, this->pacing_bitrate * 1.2, TRUE);
  _change_overused_stage_to(this, STAGE_RELEASED);
done:
  return;
}

void
_released_stage(
    SubflowRateController *this)
{
  //send back to congested stage
  if(2. < _corrH(this)){
    _reduce(this, TRUE);
    _change_overused_stage_to(this, STAGE_RELEASED);
    goto done;
  }

  //until it not goes down below to 1.5 not open it further
  if(1.5 < _corrH(this)){
    goto done;
  }

  //open it more if we have bytes in the queue
  if(_mt0(this)->bytes_in_queue){
    _set_pacing_bitrate(this, this->pacing_bitrate * 1.2, TRUE);
    goto done;
  }
  _disable_pacing(this);
  _set_adjustment_aim(this, BITRATE_UP);
  _transit_to(this, STATE_STABLE);
done:
  _add_target_point(this, _RR(this));
}

void _change_overused_stage_to(
    SubflowRateController *this,
    Stage target)
{
  switch(target){
     case STAGE_RESTRICTED:
       this->stage = _restricted_stage;
     break;
     case STAGE_RELEASED:
       this->stage = _released_stage;
     break;
   }
}

void
_overused_state(
    SubflowRateController *this)
{
  //common conditions
  if(_rlost(this) || _discard(this)){
    if(_state_t1(this) == STATE_OVERUSED){
      _transit_to(this, STATE_STABLE);
      goto done;
    }
    if(_discard(this) && _lost(this)){
      _reduce(this, TRUE);
      DISABLE_LINE  _undershoot(this, TRUE);
    }else{
      _reduce(this, TRUE);
      DISABLE_LINE  _undershoot(this, FALSE);
    }
    goto done;
  }

  //Execute stage
  this->stage(this);

//
//  if(2. < _corrH(this)){
//    //the inflection was not happened
//    if(1. <= _dCorr(this)){
//      _undershoot(this, TRUE);
//    }
//    goto done;
//  }
//
//
//  if(1.1 < _sCorr(this) || 1.2 < (gdouble)_RR(this) / (gdouble)_SR(this)){
//    //Too many bytes sent regarding to the sending rate;
//    //Either we mitigate the bytes must be sent or increase the pacing rate
//    goto done;
//  }
//
//  if(_mt0(this)->bytes_in_queue){
//    _set_pacing_bitrate(this, this->pacing_bitrate * 1.2, TRUE);
//    goto done;
//  }
//
//  _disable_pacing(this);
//  _add_target_point(this, _RR(this));
//  _set_adjustment_aim(this, BITRATE_UP);
//  _transit_to(this, STATE_STABLE);
done:
  return;
}

void
_stable_state(
    SubflowRateController *this)
{
  if(_lost(this)){
    if(_rlost(this)){
      _reduce(this, TRUE);
      DISABLE_LINE  _undershoot(this, TRUE);
      _transit_to(this, STATE_OVERUSED);
    }
    goto done;
  }
  if(_rdiscard(this)){
    _reduce(this, TRUE);
    DISABLE_LINE  _undershoot(this, TRUE);
    _transit_to(this, STATE_OVERUSED);
    goto done;
  }
  if(_corrH(this) > 1.1){
    if(_state_t1(this) != STATE_OVERUSED){
      _reduce(this, TRUE);
      DISABLE_LINE   _undershoot(this, TRUE);
      _transit_to(this, STATE_OVERUSED);
    }
    goto done;
  }

  if(1.025 < _delcorr(this)){
    _set_adjustment_aim(this, BITRATE_DOWN);
    goto done;
  }

  if(_mt0(this)->bitrate_aim != BITRATE_UP){
    _set_adjustment_aim(this, BITRATE_UP);
    goto done;
  }

  //  if(_rateCorr(this) < .9 || _maxCorr(this) < .9){
  if(_rateCorr(this) < .9 || _maxCorr(this) < .9){
    goto done;
  }
  if(_tCorr(this) < .9 || 1.1 < _tCorr(this)){
    goto done;
  }
  _set_adjustment_aim(this, BITRATE_STAY);
  _setup_monitoring(this);
  _transit_to(this, STATE_MONITORED);
done:
  return;
}

void
_monitored_state(
    SubflowRateController *this)
{
  if(_rlost(this) || _rdiscard(this)){
    _reduce(this, TRUE);
    DISABLE_LINE  _undershoot(this, TRUE);
    _transit_to(this, STATE_OVERUSED);
    goto done;
  }
  if(_lost(this) || _discard(this)){
    _set_adjustment_aim(this, BITRATE_DOWN);
    _transit_to(this, STATE_STABLE);
    goto done;
  }
  if(_corrH(this) > 1.5){
    _reduce(this, TRUE);
    DISABLE_LINE _undershoot(this, TRUE);
    _transit_to(this, STATE_OVERUSED);
    goto done;
  }
  if(_corrH(this) > 1.1){
    _set_adjustment_aim(this, BITRATE_DOWN);
    _transit_to(this, STATE_STABLE);
    goto done;
  }

  _add_target_point(this, _SR(this) + this->monitored_bitrate);
  _set_adjustment_aim(this, BITRATE_UP);
  _transit_to(this, STATE_STABLE);
done:
  _disable_monitoring(this);
  return;
}

void _reduce(SubflowRateController *this, gboolean disable_controlling)
{
  gint32 undershoot_rate = 0;
  if(_state(this) == STATE_OVERUSED){
    _change_target_bitrate(this, _SR(this) * BETA);
    _set_pacing_bitrate(this, _SR(this) * BETA, TRUE);
    goto exit;
  }

  _change_target_bitrate(this, _RR(this));
  undershoot_rate = _mt0(this)->discard * 2;
  if(undershoot_rate < this->monitored_bitrate){
    undershoot_rate = 0;
    goto done;
  }
  if(_SR(this) < undershoot_rate){
    undershoot_rate = _SR(this) * .5;
    goto done;
  }
done:
  _set_pacing_bitrate(this, _RR(this) - undershoot_rate, TRUE);
exit:
  _change_overused_stage_to(this, STAGE_RESTRICTED);
  _set_adjustment_aim(this, BITRATE_STAY);
  if(disable_controlling){
    _disable_controlling(this);
  }

  return;
}

void _undershoot(SubflowRateController *this, gboolean disable_controlling)
{
  gint32 dSR = 0; //delta SR;
//  gint32 TSR = 0; //Target SR;
  gint32 pGP = 0; //Picked GP
  gint32 max_dSR, min_dSR;
  min_dSR = _SR(this) * .1;
  max_dSR = (_SR(this) + this->monitored_bitrate) * (1.-BETA);
  //
  if(_GP_t1(this) < _SR(this)){
    dSR = _SR(this) - _GP_t1(this);
    pGP = _GP_t1(this);
  }else if(_GP(this) < _SR(this)){
    dSR = _SR(this) - _GP(this);
    pGP = _GP(this);
  }else{
    dSR = .1 * _SR(this);
  }

  dSR*=2;
  if(_SR(this) < dSR){
    if(_state(this) == STATE_OVERUSED){
      dSR = max_dSR;
    }else{
      dSR = 0;
    }
  }

  numstracker_reset(this->target_points);
  if(pGP){
    _add_target_point(this, pGP);
  }
//done:
  dSR = MAX(min_dSR, MIN(max_dSR, dSR)) + (_TR(this) - _SR(this));
  _print_undershoot(this, pGP, dSR);
  _reset_monitoring(this);
  _set_adjustment_aim(this, BITRATE_STAY);
//  g_print("Undershooting dSR: %d\n", dSR);
  _change_target_bitrate(this, -1*dSR);
  _set_pacing_bitrate(this, _TR(this), TRUE);
  if(disable_controlling){
    _disable_controlling(this);
  }
  return;
}


void
_transit_to(
    SubflowRateController *this,
    State target)
{

  switch(target){
    case STATE_OVERUSED:
      mprtps_path_set_congested(this->path);
      this->controller = _overused_state;
    break;
    case STATE_STABLE:
      mprtps_path_set_non_congested(this->path);
      this->controller = _stable_state;
    break;
    case STATE_MONITORED:
      this->controller = _monitored_state;
    break;
  }
  _mt0(this)->state = target;
}


void _set_pacing_bitrate(SubflowRateController *this,
                         guint32 pacing_bitrate,
                         gboolean enable_pacing)
{
  this->pacing_bitrate = MAX(this->cwnd_min, pacing_bitrate);
  mprtps_path_set_pacing_bitrate(this->path,
                                 this->pacing_bitrate,
                                 this->packet_obsolation_treshold);

  _set_path_pacing(this, enable_pacing);
}

void _set_path_pacing(SubflowRateController *this, gboolean enable_pacing)
{
  this->path_is_paced = enable_pacing;
  mprtps_path_set_pacing(this->path, enable_pacing);
}

gdouble _adjust_bitrate(SubflowRateController *this)
{
  gdouble drate = 0; //delta rate
  gdouble actual_rate, min_target_rate, max_target_rate, target_rate;
  gdouble approximation = 1.;
  min_target_rate = MAX(this->target_bitrate * .8, this->min_target_point);
  max_target_rate = MIN(this->target_bitrate * 1.2, this->max_target_point);

  actual_rate = _TR(this);
  if(_bitrate_aim(this) == BITRATE_DOWN){
    target_rate = min_target_rate;
    //    acceleration = (gdouble)RATE_ADJUST_INTERVAL/(gdouble)GST_SECOND;
    approximation = .5;
  }else if(_bitrate_aim(this) == BITRATE_UP){
    target_rate = max_target_rate;
    approximation = .25;
  }else{
    target_rate = _TR(this);
  }

  drate =  (target_rate - actual_rate);
//  g_print("(New target rate: %f - current target rate: %d) / 2 = %f\n", target_rate, this->target_bitrate, drate);
  drate *= approximation;
//  g_print("dRate * acceleration %f = %f\n", acceleration, drate);
  drate *= (1 - MAX(-.1, MIN(.1, this->BiF_off_avg)));
//  g_print("dRate * correction %f = %f\n", (1 - MAX(-.1, MIN(.1, this->BiF_off_avg))), drate);

  return actual_rate + drate;
}

void _disable_controlling(SubflowRateController *this)
{
  GstClockTime interval;
  interval = _mt0(this)->time - _mt1(this)->time;
  this->disable_controlling = _now(this) +  MIN(2 * GST_SECOND, MAX(1.5 * GST_SECOND, interval));
}

gboolean _monitoring_is_allowed(SubflowRateController *this)
{
  if(_now(this) - GST_SECOND < this->last_congestion_detected){
    return FALSE;
  }
  if(.1 < _mt0(this)->owd_trend){
    return FALSE;
  }
  return TRUE;
}

void _reset_monitoring(SubflowRateController *this)
{
  this->monitoring_interval = 0;
  this->monitoring_started = 0;
  this->monitored_bitrate = 0;
}

void _setup_monitoring(SubflowRateController *this)
{
  guint interval;
  gdouble plus_rate = 0, scl = 0;
  if(_SR(this) * .9 <  this->last_congestion_point)
  {
    plus_rate = this->last_congestion_point - _SR(this);
    if(0. < plus_rate){
      interval = _calculate_monitoring_interval(this, plus_rate);
      goto done;
    }
  }
  scl =  (gdouble) this->min_rate;
  scl /= (gdouble)(_SR(this) - this->min_rate * .5);
  scl *= scl;
//  scl *= 4.;
  scl = MIN(2., MAX(1./14., scl));
  plus_rate = _SR(this) * scl;
  interval = _calculate_monitoring_interval(this, plus_rate);
done:
  g_print (
    "####################### S%d Monitoring interval ################################\n"
    "scl_1: %-5.3f / %5.3f  = %5.3f|\n"
    "scl_2: %-5.3f * %5.3f  = %5.3f|\n"
    "scl: %-5.3f| plus_rate: %-5.3f|\n"
    "######################################################################################\n",
    this->id,
    (gdouble) this->min_rate, (gdouble)(_SR(this) - this->min_rate * .5),
    (gdouble) this->min_rate / (gdouble)(_SR(this) - this->min_rate * .5),

    (gdouble) this->min_rate / (gdouble)(_SR(this) - this->min_rate * .5),
    (gdouble) this->min_rate / (gdouble)(_SR(this) - this->min_rate * .5),
    (gdouble) this->min_rate / (gdouble)(_SR(this) - this->min_rate * .5) *
    (gdouble) this->min_rate / (gdouble)(_SR(this) - this->min_rate * .5),

    scl, plus_rate
    );

  if(0 < this->monitoring_interval){
    if(this->monitoring_interval < 10) ++this->monitoring_interval;
    interval = MAX(this->monitoring_interval, interval);
  }
  this->monitoring_interval = MAX(2, MIN(14, interval));
  _set_monitoring_interval(this, interval);
}

void _set_monitoring_interval(SubflowRateController *this, guint interval)
{
  this->monitoring_started = _now(this);
  if(interval > 0)
    this->monitored_bitrate = (gdouble)_actual_rate(this) / (gdouble)interval;
  else
    this->monitored_bitrate = 0;
  mprtps_path_set_monitor_interval(this->path, interval);
  return;
}

guint _calculate_monitoring_interval(SubflowRateController *this, guint32 desired_bitrate)
{
  gdouble actual, target, rate;
  guint monitoring_interval = 0;
  if(desired_bitrate <= 0){
     goto exit;
   }
  actual = MIN(_SR(this), _TR(this));
  target = actual + (gdouble) desired_bitrate;
  rate = target / actual;

  if(rate > 2.) monitoring_interval = 2;
  else if(rate > 1.5) monitoring_interval = 3;
  else if(rate > 1.25) monitoring_interval = 4;
  else if(rate > 1.2) monitoring_interval = 5;
  else if(rate > 1.16) monitoring_interval = 6;
  else if(rate > 1.14) monitoring_interval = 7;
  else if(rate > 1.12) monitoring_interval = 8;
  else if(rate > 1.11) monitoring_interval = 9;
  else if(rate > 1.10) monitoring_interval = 10;
  else if(rate > 1.09) monitoring_interval = 11;
  else if(rate > 1.08) monitoring_interval = 12;
  else if(rate > 1.07) monitoring_interval = 13;
  else  monitoring_interval = 14;

exit:
  return monitoring_interval;
}


static void _iterator_process(gpointer data, gdouble owd_fraction)
{
  RData *rdata = data;
  gdouble x0;
  x0 = owd_fraction - rdata->avg;
  rdata->a0 += x0 * x0;
  rdata->a1 += x0 * rdata->x1;
  rdata->x1 = x0;
}

void _set_owd_trend(SubflowRateController *this)
{
  RData rdata;
  gdouble trend;
  rdata.a0 = rdata.a1 = rdata.x1 = 0.;
  floatnumstracker_get_stats(this->owd_fraction_hist, NULL, &rdata.avg);
  floatnumstracker_iterate(this->owd_fraction_hist, _iterator_process, &rdata);
  if(rdata.a0 <= 0.) goto done;
  trend = MAX(0.0f, MIN(1.0f, (rdata.a1 / rdata.a0)*this->owd_fraction_avg));
  _mt0(this)->owd_trend = trend;
done:
  return;
}

gdouble _get_off_target(SubflowRateController *this)
{
  gdouble off_target;
  off_target = (gdouble)((gdouble)_mt0(this)->ltt_delays_target - (gdouble)_mt0(this)->recent_delay) / (gfloat)_mt0(this)->ltt_delays_target;
  return off_target;
}
//
//gboolean _cwnd_can_increase(SubflowRateController *this)
//{
//  gfloat alpha = 1.25f+2.75f*(1.0f-this->owd_trend_mem);
//  return this->cwnd <= alpha*_mt0(this)->bytes_in_flight_ack;
//}

void _change_target_bitrate(SubflowRateController *this, gint32 new_target)
{
  gint32 actual_target = _TR(this);
  gint32 delta;
//  delta = 0;
  delta=new_target - actual_target;
  if(0 < this->max_rate){
    new_target = MIN(new_target, this->max_rate);
  }
  if(0 < this->min_rate){
    new_target = MAX(new_target, this->min_rate);
  }
//  g_print("current target: %d delta: %d new target: %d\n",
//          _TR(this), delta, new_target);
  _mt0(this)->delta_rate = delta;
  this->target_bitrate = new_target;
}

void _ltt_delays_target_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata)
{
  SubflowRateController *this = data;
  _mt0(this)->ltt_delays_target = pdata->percentile;
}


void _ltt_delays_th_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata)
{
  SubflowRateController *this = data;
  _mt0(this)->ltt_delays_th = pdata->percentile;
}

void _target_points_max_pipe(gpointer data, guint64 value)
{
  SubflowRateController *this = data;
  this->max_target_point = MAX(value, this->min_rate);
}

void _target_points_min_pipe(gpointer data, guint64 value)
{
  SubflowRateController *this = data;
  this->min_target_point = MAX(value, this->min_rate);
}


void _bights_in_flight_max_pipe(gpointer data, guint64 value)
{
  SubflowRateController *this = data;
  _mt0(this)->max_bytes_in_flight = value;

}

void _print_mupdate_state(SubflowRateController *this)
{
  g_print (
          "############ S%d | State: %-2d | Disable time %lu | Ctrled: %d #################\n"
          "rlost:      %-10d| rdiscard:%-10d| lost:    %-10d| discard: %-10d|\n"
          "corrH:      %-10.3f| target:  %-10.3f| rdelay:  %-10.3f| Del_cor: %-10.3f|\n"
          "GP:         %-10d| SR:      %-10d| RR:      %-10d| rateCor: %-10f|\n"
          "BiF_havg:   %-10.3f| BiF_avg: %-10.3f| BiF_off: %-10.3f| Boff_avg:%-10.3f|\n"
          "target_br:  %-10d| min_tbr: %-10d| max_tbr: %-10d| sCorr:   %-10.3f|\n"
          "br_aim:     %-10d| q_bits:  %-10d| BiFCorr: %-10.3f| dCorr:   %-10.3f|\n"
          "mon_br:     %-10d| mon_int: %-10d| maxCorr: %-10.3f| lc_rate: %-10d|\n"
          "######################################################################################\n",
          this->id, _state(this),
          this->disable_controlling > 0 ? GST_TIME_AS_MSECONDS(this->disable_controlling - _now(this)) : 0,
          _mt0(this)->controlled,

          _rlost(this),_rdiscard(this),_lost(this),_discard(this),

          _corrH(this),
          (gdouble)GST_TIME_AS_MSECONDS(_mt0(this)->ltt_delays_target),
          (gdouble)_mt0(this)->recent_delay / (gdouble) GST_MSECOND,
          //_mt0(this)->off_target,
          _delcorr(this),

          _GP(this), _SR(this), _RR(this), _rateCorr(this),

          this->BiF_ested_avg, this->BiF_acked_avg,
          _mt0(this)->BiF_off,  this->BiF_off_avg,

          this->target_bitrate, this->min_target_point,
          this->max_target_point, _sCorr(this),

          _mt0(this)->bitrate_aim, _mt0(this)->bytes_in_queue * 8,
          _BiFCorr(this), _dCorr(this),

          this->monitored_bitrate, this->monitoring_interval,
          _maxCorr(this), this->last_congestion_point

          );

  //ToDO: elliminate this
  if(0 && _monitoring_is_allowed(this)) g_print("HAHA");
}


void _print_undershoot(SubflowRateController *this,
                       gint32 picked_GP,
                       gint32 drate)
{
  g_print (
          "############################### S%d Undershooted #######################################\n"
          "lc_point: %-10d| pickd_GP: %-10d| dRate:   %-10d|\n"
          "######################################################################################\n",
          this->id,
          this->last_congestion_point,
          picked_GP,
          drate
          );

  //ToDO: elliminate this
  if(0 && _monitoring_is_allowed(this)) g_print("HAHA");
}

#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef THIS_READLOCK
#undef THIS_READUNLOCK
