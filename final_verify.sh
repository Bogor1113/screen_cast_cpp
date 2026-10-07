#!/bin/bash
set -e
cd /d/screen_cast_cpp/build/Release
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
taskkill //F //IM powershell.exe 2>&1 | head -1 || true
sleep 1
# 白色对照窗口(教师端桌面上)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File /d/screen_cast_cpp/whitewin.ps1 > /tmp/fw.log 2>&1 &
sleep 3
# 教师端 GUI(自动推流)
./teacher_gui.exe 127.0.0.1 > /tmp/fv_t.log 2>&1 &
T=$!
sleep 4
# 学生端 GUI
./client_gui.exe 127.0.0.1 > /tmp/fv_c.log 2>&1 &
P=$!
sleep 8
echo '=== teacher_gui 是否在推流 ==='
tr -d '\r' < /tmp/fv_t.log | grep -cE '使用编码器' || true
echo '=== client 标题 ==='
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "(Get-Process client_gui -ErrorAction SilentlyContinue).MainWindowTitle" 2>/dev/null
echo '=== client 窗口像素(白窗应可见 + 顶部彩条) ==='
powershell.exe -NoProfile -ExecutionPolicy Bypass -File /d/screen_cast_cpp/dump_grid.ps1 -ProcName client_gui 2>/dev/null
echo '=== client 日志尾部 ==='
tr -d '\r' < /tmp/fv_c.log | tail -1
kill -0 $P 2>/dev/null && echo 'CLIENT_ALIVE' || echo 'CLIENT_DEAD'
taskkill //F //IM powershell.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
rm -f /tmp/fv_t.log /tmp/fv_c.log /tmp/fw.log C:/crash_dump.txt
