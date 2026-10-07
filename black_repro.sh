#!/bin/bash
set -e
cd /d/screen_cast_cpp/build/Release
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
taskkill //F //IM teacher_cli.exe 2>&1 | head -1 || true
taskkill //F //IM client_console.exe 2>&1 | head -1 || true
sleep 1
# 教师端 GUI(用户的实际用法)
./teacher_gui.exe 127.0.0.1 > /tmp/br_t.log 2>&1 &
T=$!
sleep 4
# 学生端 GUI
./client_gui.exe 127.0.0.1 > /tmp/br_c.log 2>&1 &
P=$!
sleep 8
echo '=== teacher_gui 日志(是否在推流) ==='
tr -d '\r' < /tmp/br_t.log | grep -E 'capture|编码器|清晰度' | head -5
echo '=== client 日志(解码是否正常) ==='
tr -d '\r' < /tmp/br_c.log | tail -2
echo '=== client 窗口状态 ==='
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-Process client_gui -ErrorAction SilentlyContinue | Select-Object Id,MainWindowHandle,MainWindowTitle,Responding | Format-List" 2>/dev/null
echo '=== client 窗口像素转储(8x5 网格) ==='
powershell.exe -NoProfile -ExecutionPolicy Bypass -File /d/screen_cast_cpp/dump_grid.ps1 -ProcName client_gui 2>/dev/null || echo '(转储失败)'
kill -0 $P 2>/dev/null && echo 'CLIENT_ALIVE' || echo 'CLIENT_DEAD'
taskkill //F //IM teacher_gui.exe 2>&1 | head -1 || true
taskkill //F //IM client_gui.exe 2>&1 | head -1 || true
rm -f /tmp/br_t.log /tmp/br_c.log C:/crash_dump.txt
