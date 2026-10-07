Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class EW {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$list = New-Object System.Collections.ArrayList
$cb = [EW+EnumProc]{
    param($h, $l)
    $cls = New-Object System.Text.StringBuilder 128
    [void][EW]::GetClassNameW($h, $cls, 128)
    $txt = New-Object System.Text.StringBuilder 128
    [void][EW]::GetWindowTextW($h, $txt, 128)
    $pid2 = 0
    [void][EW]::GetWindowThreadProcessId($h, [ref]$pid2)
    $vis = [EW]::IsWindowVisible($h)
    [void]$list.Add(("cls={0} vis={1} pid={2} txt={3}" -f $cls.ToString(), $vis, $pid2, $txt.ToString()))
    return $true
}
[void][EW]::EnumWindows($cb, [IntPtr]::Zero)
foreach ($e in $list) { Write-Output $e }
