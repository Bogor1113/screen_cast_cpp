# version_bump.ps1 -- 版本自增(共享脚本, build.bat / pack.ps1 共用)
# 版本规则(与历史 pack.ps1 行为一致):
#   已有 dist\version.txt          -> 次版本 +1 (1.0 -> 1.1), 回写 version.h 与 version.txt
#   首次(无 dist\version.txt)      -> 使用 version.h 当前版本, 不递增
#   -SkipBump                      -> 只读不改(不写 version.h; 但会同步 version.txt 为当前版本)
# 产物:
#   version.h       回写两个宏(UTF-8 BOM, 保留其余内容与换行)
#   dist\version.txt 记录当前版本(无 BOM, 供构建/打包脚本读取)
# 输出:
#   Write-Output 最终版本号(调用方从 dist\version.txt 读取即可, 不必依赖 stdout)
param(
    [switch]$SkipBump
)
$ErrorActionPreference = "Stop"

$Root       = Split-Path -Parent $PSScriptRoot      # scripts\ -> 项目根
$DistDir    = Join-Path $Root "dist"
$VersionH   = Join-Path $Root "version.h"
$VersionFile = Join-Path $DistDir "version.txt"

function Parse-Version([string]$v) {
    $m = [regex]::Match($v, '^(\d+)\.(\d+)$')
    if (-not $m.Success) { throw "版本格式非法: '$v' (应为 X.Y)" }
    return @{ major = [int]$m.Groups[1].Value; minor = [int]$m.Groups[2].Value }
}
function Format-Version($p) { return "$($p.major).$($p.minor)" }
function Read-VersionH {
    if (-not (Test-Path $VersionH)) { return "1.0" }
    $hdr = Get-Content $VersionH -Raw -Encoding UTF8
    $m = [regex]::Match($hdr, 'SCREENCAST_VERSION_WS\s+L"(\d+\.\d+)"')
    if (-not $m.Success) { $m = [regex]::Match($hdr, 'SCREENCAST_VERSION_STR\s+"(\d+\.\d+)"') }
    if ($m.Success) { return $m.Groups[1].Value }
    return "1.0"
}

if (-not $SkipBump) {
    if (Test-Path $VersionFile) {
        # 已有打包/构建记录: 次版本 +1
        $ver = Parse-Version ((Get-Content $VersionFile -Raw -Encoding UTF8).Trim())
        $old = Format-Version $ver
        $ver.minor += 1
        $version = Format-Version $ver
        Write-Host "版本自增: $old -> $version" -ForegroundColor Cyan
    } else {
        # 首次: 使用 version.h 当前版本
        $version = Read-VersionH
        Write-Host "首次构建, 使用当前版本: $version" -ForegroundColor Cyan
    }
    # 回写 version.h(UTF-8 BOM): 下次构建在此基础上 +1
    $hdr = Get-Content $VersionH -Raw -Encoding UTF8
    $hdr = [regex]::Replace($hdr, 'SCREENCAST_VERSION_STR\s+"[^"]*"', "SCREENCAST_VERSION_STR `"$version`"")
    $hdr = [regex]::Replace($hdr, 'SCREENCAST_VERSION_WS\s+L"[^"]*"',  "SCREENCAST_VERSION_WS  L`"$version`"")
    [IO.File]::WriteAllText($VersionH, $hdr, (New-Object System.Text.UTF8Encoding($true)))
} else {
    # 只读模式: 不递增, 不写 version.h; 仍同步 version.txt 记录当前版本
    $version = Read-VersionH
    Write-Host "SkipBump: 使用当前版本 $version (version.h 不变)" -ForegroundColor Cyan
}

# 始终记录当前版本到 dist\version.txt(下次递增的基准)
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
[IO.File]::WriteAllText($VersionFile, $version, (New-Object System.Text.UTF8Encoding($false)))

Write-Output $version
