# Print client window title (proves stream is live)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QS3 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$list = New-Object System.Collections.ArrayList
$cb = [QS3+EnumProc]{
    param($h, $l)
    $cls = New-Object System.Text.StringBuilder 128
    [void][QS3]::GetClassNameW($h, $cls, 128)
    if ($cls.ToString() -eq "ScreenCastClient") {
        $txt = New-Object System.Text.StringBuilder 256
        [void][QS3]::GetWindowTextW($h, $txt, 256)
        [void]$list.Add("CLIENT: " + $txt.ToString())
    }
    return $true
}
[void][QS3]::EnumWindows($cb, [IntPtr]::Zero)
foreach ($e in $list) { Write-Output $e }
