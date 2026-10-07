$ErrorActionPreference = 'Continue'
# Multicast concurrency test: teacher_cli -> 239.255.1.1 (adaptive), 4x client_console + 1x client_gui join the group
$dir = 'D:\screen_cast_cpp\build'
$out = 'D:\screen_cast_cpp\qa_out\multi'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$log = "$out\multi_log.txt"
Set-Location $dir

Get-Process -Name teacher_cli, client_gui, client_console, teacher_gui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Remove-Item "$dir\client_crash.txt","$dir\client_crash.dmp","$dir\crash_dump.txt" -ErrorAction SilentlyContinue
Remove-Item $out\* -Force -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class MultiWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
function Has-VisibleWindow($procId) {
  $found = $false
  $cb = [MultiWin+EnumProc]{ param($h, $l)
    $p = [uint32]0; [void][MultiWin]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $procId -and [MultiWin]::IsWindowVisible($h)) { $script:found = $true; return $false }
    return $true }
  [void][MultiWin]::EnumWindows($cb, [IntPtr]::Zero)
  return $found
}
function Msg($s) { Write-Host $s; Add-Content $log $s }

Msg "=== multi-client start $(Get-Date -Format HH:mm:ss) ==="
$t = Start-Process -FilePath "$dir\teacher_cli.exe" -ArgumentList '239.255.1.1','0','d','h','--adaptive' -RedirectStandardOutput "$out\teacher_out.txt" -RedirectStandardError "$out\teacher_err.txt" -PassThru
Start-Sleep -Seconds 3

$clients = @()
for ($i = 0; $i -lt 4; $i++) {
  $cc = Start-Process -FilePath "$dir\client_console.exe" -ArgumentList '127.0.0.1','239.255.1.1' -RedirectStandardOutput "$out\cc$i.txt" -RedirectStandardError "$out\cce$i.txt" -PassThru
  $clients += $cc
}
$gui = Start-Process -FilePath "$dir\client_gui.exe" -ArgumentList '127.0.0.1','239.255.1.1' -RedirectStandardOutput "$out\gui.txt" -RedirectStandardError "$out\guie.txt" -PassThru
Start-Sleep -Seconds 8

$startMs = [DateTime]::Now
$fails = @()
$peakWS = 0
# run ~12 minutes; restart teacher once at t=300s to verify all clients auto-recover via multicast
$restartDone = $false

for ($min = 0; $min -lt 12; $min++) {
  for ($q = 0; $q -lt 3; $q++) {
    Start-Sleep -Seconds 20
    $el = [int](([DateTime]::Now - $startMs).TotalSeconds)
    if (-not $restartDone -and $el -ge 300) {
      Msg "t=${el}s KILL teacher (multicast restart exercise)"
      Stop-Process -Id $t.Id -Force -ErrorAction SilentlyContinue
      Start-Sleep -Seconds 5
      $t = Start-Process -FilePath "$dir\teacher_cli.exe" -ArgumentList '239.255.1.1','0','d','h','--adaptive' -RedirectStandardOutput "$out\teacher_out_2.txt" -RedirectStandardError "$out\teacher_err_2.txt" -PassThru
      $restartDone = $true
      Msg "t=${el}s teacher restarted"
      continue
    }
    $line = "t=${el}s"
    foreach ($cl in $clients) {
      $p = Get-Process -Id $cl.Id -ErrorAction SilentlyContinue
      if (-not $p) { $line += " | cc$([array]::IndexOf($clients,$cl))-DEAD"; $fails += "t=${el}s console-dead" }
      else {
        $ws = [int]($p.WorkingSet64/1MB)
        if ($ws -gt $peakWS) { $peakWS = $ws }
        $line += " | cc$([array]::IndexOf($clients,$cl))-alive ws=${ws}MB"
      }
    }
    $gp = Get-Process -Id $gui.Id -ErrorAction SilentlyContinue
    if (-not $gp) { $line += " | gui-DEAD"; $fails += "t=${el}s gui-dead" }
    elseif ($gp.Responding -eq $false) { $line += " | gui-UNRESP"; $fails += "t=${el}s gui-unresponsive" }
    else { $line += " | gui-alive resp responding ws=$([int]($gp.WorkingSet64/1MB))MB" }
    $tp = Get-Process -Id $t.Id -ErrorAction SilentlyContinue
    if (-not $tp) { $line += " | teacher-DEAD"; $fails += "t=${el}s teacher-dead" }
    $crash = (Test-Path "$dir\client_crash.txt") -or (Test-Path "$dir\client_crash.dmp")
    if ($crash) { $fails += "t=${el}s crash-file" }
    Msg $line
  }
  # every minute dump one console client's receive stats as progress evidence
  if (Test-Path "$out\cc0.txt") {
    $tail = (Get-Content "$out\cc0.txt" | Select-Object -Last 1)
    Msg "  [cc0 progress] $tail"
  }
  if (Test-Path "$out\teacher_out.txt") {
    $tstat = (Get-Content "$out\teacher_out.txt" | Select-Object -Last 1)
    Msg "  [teacher] $tstat"
  }
}

Msg "=== multi-client end $(Get-Date -Format HH:mm:ss) ==="
Stop-Process -Id $gui.Id -Force -ErrorAction SilentlyContinue
foreach ($cl in $clients) { Stop-Process -Id $cl.Id -Force -ErrorAction SilentlyContinue }
Stop-Process -Id $t.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$crashFinal = (Test-Path "$dir\client_crash.txt") -or (Test-Path "$dir\client_crash.dmp")
$summary = @"
MULTI-CLIENT SUMMARY (~12 min, multicast 239.255.1.1)
teacher restart cycles: $(if($restartDone){1}else{0})
clients: 4x console + 1x gui
console WS peak: $peakWS MB
failures: $($fails.Count)
$(if($fails){"FAILS: " + ($fails -join '; ')}else{"no failures"})
crash files: $crashFinal
"@
Add-Content $log $summary
Write-Host ""
Write-Host $summary
Write-Host "MULTI-DONE"
