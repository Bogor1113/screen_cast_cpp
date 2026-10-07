$ErrorActionPreference = 'Continue'
# 30-minute stress test: teacher_cli(adaptive) + client_gui on 127.0.0.1
# snapshots every 30s: process alive / GUI responding / CPU / working set
# periodic teacher restart cycles to exercise auto-reconnect
$dir = 'D:\screen_cast_cpp\build'
$out = 'D:\screen_cast_cpp\qa_out\stress'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$log = "$out\stress_log.txt"
$sum = "$out\stress_summary.txt"
Set-Location $dir

Get-Process -Name teacher_cli, client_gui, teacher_gui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Remove-Item "$dir\client_crash.txt","$dir\client_crash.dmp","$dir\crash_dump.txt" -ErrorAction SilentlyContinue
Remove-Item $out\* -Force -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class StressWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@

function Get-ClientTitle($pid) {
  $script:title = ""
  $cb = [StressWin+EnumProc]{ param($h, $l)
    $p = [uint32]0; [void][StressWin]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $pid -and [StressWin]::IsWindowVisible($h)) {
      $sb = New-Object System.Text.StringBuilder 300
      [void][StressWin]::GetWindowTextW($h, $sb, 300)
      $script:title = $sb.ToString()
      return $false
    }
    return $true }
  [void][StressWin]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:title
}

function Msg($s) { Write-Host $s; Add-Content $log $s }

Msg "=== stress start $(Get-Date -Format HH:mm:ss) ==="
$t = Start-Process -FilePath "$dir\teacher_cli.exe" -ArgumentList '127.0.0.1','0','d','h','--adaptive' -RedirectStandardOutput "$out\teacher_out.txt" -RedirectStandardError "$out\teacher_err.txt" -PassThru
Start-Sleep -Seconds 3
$c = Start-Process -FilePath "$dir\client_gui.exe" -ArgumentList '127.0.0.1' -RedirectStandardOutput "$out\client_out.txt" -RedirectStandardError "$out\client_err.txt" -PassThru
Start-Sleep -Seconds 5

$restartAt = @(180, 480, 900, 1500)   # seconds: kill teacher at these points to exercise auto-reconnect
$ri = 0
$startMs = [DateTime]::Now
$clientWSpeak = 0; $clientCPUpeak = 0.0; $teacherCPUpeak = 0.0
$fails = @()

for ($min = 0; $min -lt 30; $min++) {
  for ($half = 0; $half -lt 2; $half++) {
    Start-Sleep -Seconds 30
    $el = [int](([DateTime]::Now - $startMs).TotalSeconds)
    $cp = Get-Process -Id $c.Id -ErrorAction SilentlyContinue
    $tp = Get-Process -Id $t.Id -ErrorAction SilentlyContinue
    if ($cp) { $cws = [int]($cp.WorkingSet64/1MB); $ccpu = $cp.TotalProcessorTime.TotalSeconds
               if ($cws -gt $clientWSpeak) { $clientWSpeak = $cws }
               if ($ccpu -gt $clientCPUpeak) { $clientCPUpeak = $ccpu } }
    if ($tp) { $tcpu = $tp.TotalProcessorTime.TotalSeconds; if ($tcpu -gt $teacherCPUpeak) { $teacherCPUpeak = $tcpu } }
    $crash = (Test-Path "$dir\client_crash.txt") -or (Test-Path "$dir\client_crash.dmp") -or (Test-Path "$dir\crash_dump.txt")
    if ($crash) { $fails += "t=${el}s CRASH-FILE" }
    if (-not $cp) { $fails += "t=${el}s CLIENT-DEAD" }
    elseif ($cp.Responding -eq $false) { $fails += "t=${el}s GUI-UNRESPONSIVE" }
    if (-not $tp) { $fails += "t=${el}s TEACHER-DEAD" }
    $title = ""
    if ($cp) { $title = Get-ClientTitle $c.Id }
    Msg ("t={0}s clientAlive={1} responding={2} wsMB={3} cpuT={4:N1} | teacherAlive={5} cpuT={6:N1} | title=[{7}]" -f `
         $el, ($null -ne $cp), ($(if($cp){$cp.Responding}else{$false})), $cws, $ccpu, ($null -ne $tp), $tcpu, $title)
    # teacher restart cycle at scheduled points
    if ($ri -lt $restartAt.Count -and $el -ge $restartAt[$ri]) {
      Msg "t=${el}s KILL teacher for auto-reconnect exercise"
      Stop-Process -Id $t.Id -Force -ErrorAction SilentlyContinue
      Start-Sleep -Seconds 5
      $t = Start-Process -FilePath "$dir\teacher_cli.exe" -ArgumentList '127.0.0.1','0','d','h','--adaptive' -RedirectStandardOutput "$out\teacher_out_$ri.txt" -RedirectStandardError "$out\teacher_err_$ri.txt" -PassThru
      $ri++
      Msg "t=${el}s teacher restarted"
    }
  }
}

Msg "=== stress end $(Get-Date -Format HH:mm:ss) ==="
Stop-Process -Id $c.Id,$t.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$crashFinal = (Test-Path "$dir\client_crash.txt") -or (Test-Path "$dir\client_crash.dmp") -or (Test-Path "$dir\crash_dump.txt")
$aliveFinal = $null -ne (Get-Process -Id $c.Id -ErrorAction SilentlyContinue)

$summary = @"
STRESS SUMMARY (30 min)
client WS peak: $clientWSpeak MB
client CPU total: $($clientCPUpeak.ToString('N1')) s
teacher CPU total: $($teacherCPUpeak.ToString('N1')) s
restart cycles: $($restartAt.Count)
failures: $($fails.Count)
$(if($fails){"FAILS: " + ($fails -join '; ')}else{"no failures"})
crash files: $crashFinal
client still alive after kill: $aliveFinal (expected False)
"@
Add-Content $sum $summary
Write-Host ""
Write-Host $summary
Write-Host "STRESS-DONE"
