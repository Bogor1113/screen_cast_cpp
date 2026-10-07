# 点击教师端「开始推流」按钮: 用控件 ID(104=IDC_BTN_START) 定位, 避免中文编码问题
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CS3 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, int m, IntPtr wp, IntPtr lp);
}
"@
$w = [CS3]::FindWindowW("ScreenCastTeacher", $null)
if ($w -eq [IntPtr]::Zero) { Write-Output "NO_TEACHER_WIN"; exit 1 }
$btn = [CS3]::GetDlgItem($w, 104)   # IDC_BTN_START
if ($btn -eq [IntPtr]::Zero) { Write-Output "NO_BTN"; exit 1 }
[void][CS3]::SendMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
Write-Output "CLICKED"
