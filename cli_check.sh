#!/bin/bash
set -e
cd /d/screen_cast_cpp/build/Release
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_cli.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
sleep 1
./teacher_cli.exe 127.0.0.1 > /tmp/cli_t.log 2>&1 &
T=$!
sleep 2
./client_gui.exe 127.0.0.1 > /tmp/cli_c.log 2>&1 &
P=$!
sleep 6
echo '=== 教师端状态(含客户端列表) ==='
tr -d '\r' < /tmp/cli_t.log | tail -1
echo '=== client 日志尾部 ==='
tr -d '\r' < /tmp/cli_c.log | tail -1
kill -0 $P 2>/dev/null && echo 'CLIENT_ALIVE' || echo 'CLIENT_DEAD'
taskkill //F //IM teacher_cli.exe 2>&1 | head -1 || true
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
rm -f /tmp/cli_t.log /tmp/cli_c.log C:/crash_dump.txt
