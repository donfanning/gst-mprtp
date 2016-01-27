#! /bin/bash
#Test3 - Playout test
#echo "Single congestion test on veth2"
#echo "S1:100ms - 2ms"
#To attach a TBF with a sustained maximum rate of 1mbit/s, 
#a peakrate of 2.0mbit/s, 
#a 10kilobyte buffer, with a pre-bucket queue size limit calculated so the TBF causes at most 70ms of latency, 
#with perfect peakrate behavior, enter:
#tc qdisc add dev veth2 root tbf rate 1mbit burst 10kb latency 70ms peakrate 2mbit minburst 1540
#tc qdisc change dev veth2 root tbf rate 1mbit burst 10kb latency 70ms peakrate 2mbit minburst 1540

LATENCY=75
JITTER=1

#echo "Setup veth2 to 1000KBit"
BW=800
tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
for j in `seq 1 120`;
do
  echo "$BW,|"
  sleep 1
done
#2 minute

BW=200
tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
for j in `seq 1 60`;
do
  echo "$BW,|"
  sleep 1
done
#2 minute

BW=800
tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
for j in `seq 1 60`;
do
  echo "$BW,|"
  sleep 1
done
#3 minute

BW=200
tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
for j in `seq 1 30`;
do
  echo "$BW,|"
  sleep 1
done
#3.5 minute

BW=800
tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
for j in `seq 1 30`;
do
  echo "$BW,|"
  sleep 1
done
#4 minute

#echo "2 Stairs closing"
for i in `seq 1 2`;
do
  let BW=800-$i*300
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 30`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
#5 minute

#echo "4 Stairs opening"
for i in `seq 1 4`;
do
  let BW=200+$i*150
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 15`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
#6 minute

#echo "8 Stairs closing"
for i in `seq 1 8`;
do
  let BW=800-$i*75
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 7`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
for j in `seq 1 4`;
do
  echo "$BW,|"
  sleep 1
done
#7 minute

#echo "2 Stairs opening"
for i in `seq 1 2`;
do
  let BW=200+$i*300
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 30`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
#8 minute

#echo "4 Stairs closing"
for i in `seq 1 4`;
do
  let BW=800-$i*150
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 15`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
#9 minute

#echo "8 Stairs opening"
for i in `seq 1 8`;
do
  let BW=200+$i*75
  tc class change dev veth2 parent 2:2 classid 1:12 htb rate "$BW"Kbit ceil "$BW"Kbit
  tc qdisc change dev veth2 parent 2:22 netem delay "$LATENCY"ms "$JITTER"ms
  for j in `seq 1 7`;
  do
    echo "$BW,|"
    sleep 1
  done
done 
for j in `seq 1 4`;
do
  echo "$BW,|"
  sleep 1
done
#10 minute


