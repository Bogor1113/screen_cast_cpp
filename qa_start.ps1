# QA helper: start white window + teacher + client on loopback, then dump client window title (FPS/loss stats)
param([string]$TeacherIP = "127.0.0.1")
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QS {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
# 1. kill stale processes
Stop-Process -Name teacher_gui, client_gui -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# 2. white window (bright background so a black cursor box is obvious)
Start-Process powershell -ArgumentList "-NoProfile", "-File", "D:\screen_cast_cpp\whitewin.ps1"
Start-Sleep -Seconds 2

# 3. teacher (unicast to loopback) - starts streaming immediately
Start-Process "D:\screen_cast_cpp\build\Release\teacher_gui.exe" -ArgumentList $TeacherIP
Start-Sleep -Seconds 2

# 4. client (direct connect, no dialog)
Start-Process "D:\screen_cast_cpp\build\Release\client_gui.exe" -ArgumentList $TeacherIP
Start-Sleep -Seconds 6

# 5. dump client window title: contains IP / fps / loss / delay -> proves stream is live
$list = New-Object System.Collections.ArrayList
$cb = [QS+EnumProc]{
    param($h, $l)
    $cls = New-Object System.Text.StringBuilder 128
    [void][QS]::GetClassNameW($h, $cls, 128)
    if ($cls.ToString() -eq "ScreenCastClient") {
        $txt = New-Object System.Text.StringBuilder 256
        [void][QS]::GetWindowTextW($h, $txt, 256)
        [void]$list.Add("CLIENT_TITLE: " + $txt.ToString())
    }
    if ($cls.ToString() -eq "ScreenCastTeacher") {
        $txt = New-Object System.Text.StringBuilder 256
        [void][QS]::GetWindowTextW($h, $txt, 256)
        [void]$list.Add("TEACHER_TITLE: " + $txt.ToString())
    }
    return $true
}
[void][QS]::EnumWindows($cb, [IntPtr]::Zero)
foreach ($e in $list) { Write-Output $e }
