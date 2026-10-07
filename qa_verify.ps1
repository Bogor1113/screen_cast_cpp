# qa_verify.ps1 - automated QA for the user-reported client bugs
#   Q1: cursor overlay must NOT draw a black square (transparent background stays transparent)
#   Q2: at fit (zoom=100%) click-drag must NOT move the picture (drag disabled at fit)
#   Q2b: when zoomed in (>100%) click-drag MUST pan the picture (drag right -> picture slides left)
#   Q3: hovering at fit must show the default arrow; hovering zoomed-in must show IDC_HAND
# Verdicts are printed as PASS/FAIL lines; screenshots go to qa_out\
# NOTE: pure-ASCII source (PowerShell 5.1 reads .ps1 without BOM as ANSI; non-ASCII would break parsing)
param([string]$TeacherIP = "127.0.0.1")

$root = "D:\screen_cast_cpp"
$out  = Join-Path $root "qa_out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies @("System.Drawing", "System.Runtime.InteropServices") @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QV {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int ht, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr inst, int id);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int idx);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(out CURSORINFO ci);
    [DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr hdc, int x, int y, IntPtr hIcon, int w, int h, int step, IntPtr brush, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT ptScreenPos; }
    public static int CURSOR_INFO_SIZE = System.Runtime.InteropServices.Marshal.SizeOf(typeof(CURSORINFO));
    // find the longest run of bright (>200) pixels on row y; returns int[]{start, end} or null
    public static int[] WhiteRun(System.Drawing.Bitmap bmp, int y) {
        int bestStart = -1, bestEnd = -1, runStart = -1;
        for (int x = 0; x < bmp.Width; x++) {
            var c = bmp.GetPixel(x, y);
            bool bright = (c.R + c.G + c.B) > 200;
            if (bright) { if (runStart < 0) runStart = x; }
            else if (runStart >= 0) {
                int len = x - 1 - runStart;
                if (len > bestEnd - bestStart) { bestStart = runStart; bestEnd = x - 1; }
                runStart = -1;
            }
        }
        if (runStart >= 0 && bmp.Width - 1 - runStart > bestEnd - bestStart) { bestStart = runStart; bestEnd = bmp.Width - 1; }
        if (bestStart < 0) return null;
        return new int[] { bestStart, bestEnd };
    }
    // count dark (<thresh) pixels sampled every 2px (black-square / cursor detection)
    public static int CountDark(System.Drawing.Bitmap bmp, int thresh) {
        int n = 0;
        for (int y = 0; y < bmp.Height; y += 2)
            for (int x = 0; x < bmp.Width; x += 2) {
                var c = bmp.GetPixel(x, y);
                if (c.R < thresh && c.G < thresh && c.B < thresh) n++;
            }
        return n;
    }
}
"@

# ---------- helpers ----------
function Find-Wnd([string]$cls, [string]$title) {
    $script:found = [IntPtr]::Zero
    $cb = [QV+EnumProc]{ param($h, $l)
        if ($cls)   { $s = New-Object System.Text.StringBuilder 160; [void][QV]::GetClassNameW($h, $s, 160); if ($s.ToString() -eq $cls) { $script:found = $h; return $false } }
        if ($title) { $s = New-Object System.Text.StringBuilder 256; [void][QV]::GetWindowTextW($h, $s, 256); if ($s.ToString() -eq $title) { $script:found = $h; return $false } }
        return $true }
    [void][QV]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Get-Title($hwnd) { $s = New-Object System.Text.StringBuilder 256; [void][QV]::GetWindowTextW($hwnd, $s, 256); return $s.ToString() }
function Get-ClientScreenRect($hwnd) {
    $r = New-Object QV+RECT; [void][QV]::GetClientRect($hwnd, [ref]$r)
    $p = New-Object QV+POINT; [void][QV]::ClientToScreen($hwnd, [ref]$p)
    $r.L += $p.X; $r.R += $p.X; $r.T += $p.Y; $r.B += $p.Y
    return $r
}
function Snap($rect, $path) {
    $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.L, $rect.T, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return $bmp
}
function Lum($bmp, $x, $y) {
    if ($x -lt 0 -or $y -lt 0 -or $x -ge $bmp.Width -or $y -ge $bmp.Height) { return -1 }
    $c = $bmp.GetPixel($x, $y); return ($c.R + $c.G + $c.B)
}
# longest contiguous run of bright (>200) pixels on row y; returns @(start, end) or $null
function Get-WhiteRun($bmp, $y) {
    $r = [QV]::WhiteRun($bmp, $y)
    if ($null -eq $r) { return $null }
    return @($r[0], $r[1])
}
$results = New-Object System.Collections.ArrayList
function Report($name, $verdict, $detail) { [void]$results.Add("$name : $verdict : $detail"); Write-Output "$name : $verdict : $detail" }

# ---------- 0. cleanup + launch ----------
Stop-Process -Name teacher_gui, client_gui -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
$pWhite  = Start-Process powershell -ArgumentList "-NoProfile", "-File", "$root\whitewin.ps1" -PassThru
Start-Sleep -Milliseconds 500
$pTeach  = Start-Process "$root\build\Release\teacher_gui.exe" -ArgumentList $TeacherIP -PassThru
Start-Sleep -Milliseconds 500
$pClient = Start-Process "$root\build\Release\client_gui.exe" -ArgumentList $TeacherIP -PassThru
Start-Sleep -Seconds 7

$client = Find-Wnd "ScreenCastClient" $null
$white  = Find-Wnd $null "WHITE_TEST"
$teach  = Find-Wnd "ScreenCastTeacher" $null
if ($client -eq [IntPtr]::Zero) { Report "SETUP" "FAIL" "client window (ScreenCastClient) not found"; exit 1 }
if ($white  -eq [IntPtr]::Zero) { Report "SETUP" "FAIL" "white window (WHITE_TEST) not found"; exit 1 }
$ctitle = Get-Title $client
if ($ctitle -notmatch "FPS") { Report "SETUP" "FAIL" "client not receiving stream: '$ctitle'"; exit 1 }
Report "SETUP" "PASS" "client title: $ctitle"

# ---------- 1. arrange windows (non-overlapping layout) ----------
if ($teach -ne [IntPtr]::Zero) { [void][QV]::ShowWindow($teach, 6) }   # SW_MINIMIZE (teacher excluded from capture anyway)
$deskW = [QV]::GetSystemMetrics(0); $deskH = [QV]::GetSystemMetrics(1)
[void][QV]::SetWindowPos($white,  [IntPtr]::Zero, ($deskW - 900), 150, 900, 700, 0x0040)
[void][QV]::SetWindowPos($client, [IntPtr]::Zero, 20, 20, 960, 600, 0x0040)
[void][QV]::SetWindowPos($client, [IntPtr]::Zero, 20, 20, 960, 600, 0x0040)   # raise above white
Start-Sleep -Milliseconds 1000

$clientRect = Get-ClientScreenRect $client
$cW = $clientRect.R - $clientRect.L; $cH = $clientRect.B - $clientRect.T
$bs = [Math]::Min($cW / $deskW, $cH / $deskH)

# ---------- Q1: cursor overlay - black square? ----------
# teacher cursor over the white window center; client overlays the arrow there.
# fixed: sparse arrow (~100-250 dark px in the 30x30 box) / bug: solid 30x30 black box (~900 dark px)
$whiteCX = ($deskW - 450); $whiteCY = 500
[void][QV]::SetCursorPos($whiteCX, $whiteCY)
Start-Sleep -Milliseconds 900
$bmpQ1 = Snap $clientRect "$out\qa_cursor.png"
$ax = [int]($cW / 2 + ($whiteCX - $deskW / 2) * $bs)
$ay = [int]($cH / 2 + ($whiteCY - $deskH / 2) * $bs)
$dark = 0; $bgSum = 0; $bgN = 0; $minX = 99999; $maxX = -1; $minY = 99999; $maxY = -1
for ($y = $ay - 2; $y -le $ay + 32; $y++) {
    for ($x = $ax - 2; $x -le $ax + 32; $x++) {
        $l = Lum $bmpQ1 $x $y
        if ($l -lt 0) { continue }
        if ($l -lt 100) { $dark++; if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }; if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y } }
        else { $bgSum += $l; $bgN++ }
    }
}
$bgAvg = if ($bgN -gt 0) { [int]($bgSum / $bgN) } else { -1 }
$boxPx = 35 * 35; $ratio = [Math]::Round($dark / $boxPx, 2)
$bboxW = if ($maxX -ge $minX) { $maxX - $minX + 1 } else { 0 }
$bboxH = if ($maxY -ge $minY) { $maxY - $minY + 1 } else { 0 }
$solid  = if ($bboxW -gt 0 -and $bboxH -gt 0) { [Math]::Round($dark / ($bboxW * $bboxH), 2) } else { 0 }
$sideLum = Lum $bmpQ1 ($ax + 28) $ay
$q1detail = "hotspot=($ax,$ay) dark=$dark/$boxPx ratio=$ratio bbox=${bboxW}x${bboxH} solid=$solid bgAvg=$bgAvg side(+28)=$sideLum"
if ($bgAvg -ge 150) {
    if ($ratio -lt 0.55 -and $dark -ge 30) { Report "Q1 cursor-blackbox" "PASS" $q1detail }
    elseif ($ratio -gt 0.8) { Report "Q1 cursor-blackbox" "FAIL" $q1detail }
    else { Report "Q1 cursor-blackbox" "INCONCLUSIVE" $q1detail }
} else { Report "Q1 cursor-blackbox" "INCONCLUSIVE" "$q1detail (background not bright - white window not visible at hotspot)" }
$bmpQ1.Dispose()

# ---------- Q2: at fit, click-drag must NOT move the picture ----------
$bmpB = Snap $clientRect "$out\qa_drag_before.png"
$rowY = [int]($cH / 2)
$runB = Get-WhiteRun $bmpB $rowY
$bmpB.Dispose()
if ($null -eq $runB) { Report "Q2 fit-drag" "FAIL" "no white region found at row $rowY - white window not in client video" }
else {
    $cx = [int]($cW / 2); $cy = [int]($cH / 2)
    [void][QV]::PostMessageW($client, 0x0201, [IntPtr]1, [IntPtr](($cy -shl 16) -bor $cx))            # WM_LBUTTONDOWN
    for ($s = 1; $s -le 5; $s++) {
        $nx = $cx + $s * 30
        [void][QV]::PostMessageW($client, 0x0200, [IntPtr]1, [IntPtr](($cy -shl 16) -bor $nx))       # WM_MOUSEMOVE+MK_LBUTTON
        Start-Sleep -Milliseconds 30
    }
    [void][QV]::PostMessageW($client, 0x0200, [IntPtr]1, [IntPtr](($cy -shl 16) -bor ($cx + 150)))   # final
    Start-Sleep -Milliseconds 120
    [void][QV]::PostMessageW($client, 0x0202, [IntPtr]0, [IntPtr](($cy -shl 16) -bor ($cx + 150)))   # WM_LBUTTONUP
    Start-Sleep -Milliseconds 500
    $bmpA = Snap $clientRect "$out\qa_drag_after.png"
    $runA = Get-WhiteRun $bmpA $rowY
    $bmpA.Dispose()
    if ($null -eq $runA) { Report "Q2 fit-drag" "FAIL" "white region lost after drag (row $rowY)" }
    else {
        $shift = $runA[0] - $runB[0]   # must stay ~0 at fit: dragging is disabled at fit zoom
        $q2detail = "whiteLeft before=$($runB[0]) after=$($runA[0]) shift=$shift (drag +150 right at fit)"
        if ([Math]::Abs($shift) -lt 20) { Report "Q2 fit-drag" "PASS" "$q2detail (picture did NOT move at fit - bug fixed)" }
        else { Report "Q2 fit-drag" "FAIL" "$q2detail (picture moved at fit - bug reproduced)" }
    }
}
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x31, [IntPtr]0)   # WM_KEYDOWN '1' = resetView (fit centered)
Start-Sleep -Milliseconds 400

# ---------- Q2b: when zoomed in, click-drag MUST pan ----------
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x3D, [IntPtr]0)   # WM_KEYDOWN '=' = zoomIn (x1.25)
Start-Sleep -Milliseconds 150
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x3D, [IntPtr]0)   # second zoomIn -> 1.5625x
Start-Sleep -Milliseconds 400
$bmpB2 = Snap $clientRect "$out\qa_zoom_before.png"
$runB2 = Get-WhiteRun $bmpB2 $rowY
$bmpB2.Dispose()
if ($null -eq $runB2) { Report "Q2b zoom-drag" "INCONCLUSIVE" "no white region at row $rowY after zoom-in" }
else {
    [void][QV]::PostMessageW($client, 0x0201, [IntPtr]1, [IntPtr](($cy -shl 16) -bor $cx))
    for ($s = 1; $s -le 5; $s++) {
        $nx = $cx + $s * 30
        [void][QV]::PostMessageW($client, 0x0200, [IntPtr]1, [IntPtr](($cy -shl 16) -bor $nx))
        Start-Sleep -Milliseconds 30
    }
    [void][QV]::PostMessageW($client, 0x0200, [IntPtr]1, [IntPtr](($cy -shl 16) -bor ($cx + 150)))
    Start-Sleep -Milliseconds 120
    [void][QV]::PostMessageW($client, 0x0202, [IntPtr]0, [IntPtr](($cy -shl 16) -bor ($cx + 150)))
    Start-Sleep -Milliseconds 500
    $bmpA2 = Snap $clientRect "$out\qa_zoom_after.png"
    $runA2 = Get-WhiteRun $bmpA2 $rowY
    $bmpA2.Dispose()
    if ($null -eq $runA2) { Report "Q2b zoom-drag" "FAIL" "white region lost after zoomed drag (row $rowY)" }
    else {
        $shift2 = $runA2[0] - $runB2[0]
        $q2bdetail = "whiteLeft before=$($runB2[0]) after=$($runA2[0]) shift=$shift2 (drag +150 right at 1.56x)"
        if ([Math]::Abs($shift2) -ge 60) { Report "Q2b zoom-drag" "PASS" "$q2bdetail (zoomed drag pans - expected)" }
        else { Report "Q2b zoom-drag" "FAIL" "$q2bdetail (zoomed drag did not pan - bug)" }
    }
}
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x31, [IntPtr]0)   # reset to fit again
Start-Sleep -Milliseconds 400

# ---------- Q3: cursor shape (arrow at fit / hand when zoomed) ----------
$hcX = $clientRect.L + [int]($cW / 2); $hcY = $clientRect.T + [int]($cH / 2)
$hand = [QV]::LoadCursor([IntPtr]::Zero, 32649)   # IDC_HAND
function Get-CursorInfo {
    $ci = New-Object QV+CURSORINFO
    $ci.cbSize = [QV]::CURSOR_INFO_SIZE
    [void][QV]::GetCursorInfo([ref]$ci)
    return $ci
}
# Q3a: at fit -> arrow (NOT hand)
[void][QV]::SetCursorPos($hcX, $hcY)
Start-Sleep -Milliseconds 400
$ci = Get-CursorInfo
if ($ci.hCursor -eq $hand) { Report "Q3a fit-arrow" "FAIL" "hCursor=$($ci.hCursor) is IDC_HAND - should be arrow at fit" }
else { Report "Q3a fit-arrow" "PASS" "hCursor=$($ci.hCursor) != IDC_HAND (arrow at fit)" }
# Q3b: zoom in -> hand
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x3D, [IntPtr]0)
Start-Sleep -Milliseconds 150
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x3D, [IntPtr]0)
Start-Sleep -Milliseconds 400
$ciB = Get-CursorInfo
if ($ciB.hCursor -eq $hand) { Report "Q3b zoom-hand" "PASS" "hCursor=$($ciB.hCursor) equals IDC_HAND (zoomed)" }
else { Report "Q3b zoom-hand" "FAIL" "hCursor=$($ciB.hCursor) != IDC_HAND - expected hand when zoomed" }
# Q3c: reset to fit -> arrow again
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x31, [IntPtr]0)
Start-Sleep -Milliseconds 400
$ciC = Get-CursorInfo
if ($ciC.hCursor -eq $hand) { Report "Q3c reset-arrow" "FAIL" "hCursor=$($ciC.hCursor) is IDC_HAND - should be arrow after reset" }
else { Report "Q3c reset-arrow" "PASS" "hCursor=$($ciC.hCursor) != IDC_HAND (arrow after reset to fit)" }
# save the zoomed hand-cursor overlay image as evidence
$bmp3 = Snap $clientRect "$out\qa_hand_raw.png"
$g = [System.Drawing.Graphics]::FromImage($bmp3)
$hdc = $g.GetHdc()
[void][QV]::DrawIconEx($hdc, ($ciB.ptScreenPos.X - $clientRect.L), ($ciB.ptScreenPos.Y - $clientRect.T), $ciB.hCursor, 0, 0, 0, [IntPtr]::Zero, 3)
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp3.Save("$out\qa_hand.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp3.Dispose()

# ---------- bonus: wheel zoom regression ----------
$wcx = $clientRect.L + [int]($cW / 2); $wcy = $clientRect.T + [int]($cH / 2)
[void][QV]::PostMessageW($client, 0x020A, [IntPtr](120 -shl 16), [IntPtr](($wcy -shl 16) -bor $wcx))   # WM_MOUSEWHEEL +120
Start-Sleep -Milliseconds 500
$t2 = Get-Title $client
if ($t2 -match "112%") { Report "B1 wheel-zoom" "PASS" "title zoom shows 112%: $t2" }
else { Report "B1 wheel-zoom" "INFO" "title after wheel: $t2" }
[void][QV]::PostMessageW($client, 0x0100, [IntPtr]0x31, [IntPtr]0)   # reset view again

Write-Output ""
Write-Output "=== SUMMARY ==="
foreach ($r in $results) { Write-Output $r }
$results | Out-File -FilePath "$out\qa_summary.txt" -Encoding utf8
Write-Output "screenshots in: $out"
