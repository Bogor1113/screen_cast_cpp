param([string]$ProcName = "client_gui")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DG {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Drawing
$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WIN"; exit 1 }
[void][DG]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 300
$r = New-Object DG+RECT
[void][DG]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$g.Dispose()
# 8x5 网格 (跳过标题栏, 从 y=40 开始)
Write-Output ("WIN {0}x{1}@{2},{3}" -f $w, $h, $r.L, $r.T)
for ($gy = 0; $gy -lt 5; $gy++) {
    $line = ""
    for ($gx = 0; $gx -lt 8; $gx++) {
        $x = [Math]::Min($w-1, 20 + $gx * [int](($w-40)/7))
        $y = [Math]::Min($h-1, 40 + $gy * [int](($h-80)/4))
        $c = $bmp.GetPixel($x, $y)
        $line += ("[{0},{1},{2}]" -f $c.R, $c.G, $c.B)
    }
    Write-Output $line
}
# 红点检测(左上角区域)
$red = 0
for ($y = 25; $y -lt [Math]::Min(160, $h); $y++) {
    for ($x = 8; $x -lt [Math]::Min(200, $w); $x++) {
        $c = $bmp.GetPixel($x, $y)
        if ($c.R -gt ($c.G + 30) -and $c.R -gt ($c.B + 30)) { $red++ }
    }
}
# 非黑像素占比
$nb = 0
for ($i = 0; $i -lt 300; $i++) {
    $x = ($i * 137) % [Math]::Max(1,$w); $y = ($i * 89) % [Math]::Max(1,$h)
    $c = $bmp.GetPixel($x, $y)
    if (($c.R + $c.G + $c.B) -gt 90) { $nb++ }
}
$c0 = $bmp.GetPixel([Math]::Min($w-1,[int]($w/2)), [Math]::Min($h-1,[int]($h/2)))
$bmp.Dispose()
Write-Output ("REDDOT {0}px NONBLACK {1}% CENTER {2},{3},{4}" -f $red, [int]($nb*100/300), $c0.R, $c0.G, $c0.B)
