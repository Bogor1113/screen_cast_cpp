# 读取教师端客户端列表控件: ID 108=头部(人数), 109=ListBox(IP 列表)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class LBChk2 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int SendMessageW(IntPtr h, int m, IntPtr wp, IntPtr lp);
}
"@
$w = [LBChk2]::FindWindowW("ScreenCastTeacher", $null)
if ($w -eq [IntPtr]::Zero) { Write-Output "NO_TEACHER_WIN"; exit 1 }
$hdr = [LBChk2]::GetDlgItem($w, 108)    # IDC_CLIENTS
$lb  = [LBChk2]::GetDlgItem($w, 109)    # IDC_CLIENT_LB
$sb = New-Object System.Text.StringBuilder 128
if ($hdr -ne [IntPtr]::Zero) { [void][LBChk2]::GetWindowTextW($hdr, $sb, 128) }
$header = $sb.ToString()
if ($lb -eq [IntPtr]::Zero) { Write-Output ("HEADER: {0} | NO_LISTBOX" -f $header); exit 1 }
$count = [LBChk2]::SendMessageW($lb, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)   # LB_GETCOUNT
$items = @()
for ($i = 0; $i -lt $count; $i++) {
    $len = [LBChk2]::SendMessageW($lb, 0x018A, [IntPtr]$i, [IntPtr]::Zero)     # LB_GETTEXTLEN
    if ($len -le 0) { continue }
    $ts = New-Object System.Text.StringBuilder ($len + 2)
    [void][LBChk2]::SendMessageW($lb, 0x0189, [IntPtr]$i, $ts)                 # LB_GETTEXT
    $items += $ts.ToString()
}
Write-Output ("HEADER: {0} | COUNT: {1} | ITEMS: {2}" -f $header, $count, ($items -join ","))
