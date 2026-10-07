#!/bin/bash
set -e
cd /d/screen_cast_cpp/build/Release
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
sleep 1
./teacher_gui.exe 127.0.0.1 > /tmp/gc_t.log 2>&1 &
T=$!
sleep 3
powershell.exe -NoProfile -ExecutionPolicy Bypass -File /d/screen_cast_cpp/click_start.ps1 2>/dev/null | tail -1
sleep 3
./client_gui.exe 127.0.0.1 > /tmp/gc_c.log 2>&1 &
P=$!
sleep 7
echo '=== 教师端客户端列表控件 ==='
powershell.exe -NoProfile -ExecutionPolicy Bypass -File /d/screen_cast_cpp/lb_check.ps1 2>/dev/null || echo '(读取失败)'
echo '=== teacher_gui 日志 ==='
tr -d '\r' < /tmp/gc_t.log | tail -2
echo '=== client 日志 ==='
tr -d '\r' < /tmp/gc_c.log | tail -1
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
rm -f /tmp/gc_t.log /tmp/gc_c.log C:/crash_dump.txt
