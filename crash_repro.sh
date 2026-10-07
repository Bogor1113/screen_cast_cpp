#!/bin/bash
set -e
cd /d/screen_cast_cpp/build/Release
for i in 1 2 3 4; do
  rm -f crash_dump.txt
  taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
  taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
  sleep 1
  ./teacher_gui.exe 127.0.0.1 > /tmp/cr${i}_t.log 2>&1 &
  T=$!
  sleep 3
  ./client_gui.exe 127.0.0.1 > /tmp/cr${i}_c.log 2>&1 &
  P=$!
  sleep 12
  if kill -0 $T 2>/dev/null; then TS=ALIVE; else TS=CRASHED; fi
  if kill -0 $P 2>/dev/null; then PS=ALIVE; else PS=CRASHED; fi
  echo "cycle$i: teacher=$TS client=$PS"
  if [ -f crash_dump.txt ]; then echo "  >>> crash_dump: $(cat crash_dump.txt)"; fi
  CL=$(tr -d '\r' < /tmp/cr${i}_c.log | grep -E '收包' | tail -1)
  echo "  client: $CL"
  taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
  taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
  sleep 1
done
rm -f /tmp/cr*_t.log /tmp/cr*_c.log C:/crash_dump.txt
