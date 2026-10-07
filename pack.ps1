# pack.ps1 —— 一键打包 screen_cast 安装包(编译 + 版本自增 + WiX 生成 MSI + 解压版 zip)
# 用法:
#   直接双击 pack.bat 即可(自动执行本脚本)
#   或命令行: powershell -ExecutionPolicy Bypass -File pack.ps1
#   可选参数: -NoBuild 只打包, 不重新编译(直接使用 build\Release 现有产物)
#            -SkipBump 打包但不递增版本
# 版本规则:
#   首次打包: 从 version.h 读取当前版本(如 3.4), 生成 ScreenCast_v3.4.msi(MSI 版本 3.4.0)
#   后续打包: 依据 dist\version.txt 记录的上次版本, 次版本自动 +1(3.4 -> 3.5), 并回写 version.h
# 产物:
#   dist\ScreenCast_v3.4.msi                  MSI 安装包(WiX 生成, 双击安装)
#     - 安装时单选 学生端/教师端 (InstallModeDlg)
#     - 自动创建所选模式的「桌面」+「开始菜单」快捷方式
#     - 支持卸载(设置 -> 应用)与高版本覆盖升级
#   dist\ScreenCast_v3.4_解压版.zip           免安装解压版(client_gui + teacher_gui + FFmpeg DLL)
param(
    [switch]$NoBuild,
    [switch]$SkipBump,
    [string]$BuildDir = "$PSScriptRoot\build\Release",
    [string]$DistDir   = "$PSScriptRoot\dist"
)

$ErrorActionPreference = "Stop"

# ---------- 0. 版本自增(共享逻辑: scripts\version_bump.ps1, 与 build.bat 同一套规则) ----------
$bumpScript = Join-Path $PSScriptRoot "scripts\version_bump.ps1"
if (-not (Test-Path $bumpScript)) { Write-Error "缺少共享版本脚本: $bumpScript"; exit 1 }
# 注意: 不能用数组 splat (@("-SkipBump")) 传 switch 参数 —— 数组元素被当作位置参数,
# [switch] 不接受位置绑定, 会被静默忽略导致版本误自增。必须显式传 -SkipBump。
try {
    if ($SkipBump) {
        & $bumpScript -SkipBump
    } else {
        & $bumpScript
    }
} catch {
    Write-Error "版本自增失败: $($_.Exception.Message)"; exit 1
}
$version = (Get-Content (Join-Path $DistDir "version.txt") -Raw -Encoding UTF8).Trim()
$msiVersion = "$version.0"   # MSI 版本必须是 a.b.c 三段
Write-Host "本次打包版本: v$version (MSI 版本 $msiVersion)" -ForegroundColor Green

# ---------- 1. 编译(默认) ----------
if (-not $NoBuild) {
    Write-Host "开始编译 Release 版本..." -ForegroundColor Cyan
    # WHY NOT `cmake --build`: this machine's build tree is not a stock cmake setup. cmake is no longer
    # on PATH, the pip-vendored cmake 3.31 lives next to a stale cmake-4.4 module tree, and because a
    # module file there is newer than build/CMakeFiles/generate.stamp.depend, ANY `cmake --build`
    # auto-triggers a re-configure -- which then fails ("CXX compiler identification is unknown") in a
    # shell without the Visual Studio environment, and can leave the cache half-rewritten. The generated
    # .vcxproj files are already configured correctly, so MSBuild is called on them directly (the same
    # recipe used for every successful build of this project) and the "re-run cmake" stamp rule inside
    # the projects is neutralised (it is what turns a plain build into a reconfigure).
    $msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    if (-not (Test-Path $msbuild)) { Write-Error "未找到 MSBuild: $msbuild"; exit 1 }

    # Git-Bash-derived environments carry duplicate keys with different case; the MSBuild file tracker
    # then misses rebuilds. Collapse them (no-op when there are none).
    $all = [System.Environment]::GetEnvironmentVariables('Process')
    $groups = @{}
    foreach ($k in @($all.Keys)) {
        $lk = $k.ToLowerInvariant()
        if (-not $groups.ContainsKey($lk)) { $groups[$lk] = New-Object System.Collections.ArrayList }
        [void]$groups[$lk].Add($k)
    }
    foreach ($lk in $groups.Keys) {
        if ($groups[$lk].Count -gt 1) {
            $names = @($groups[$lk]); $val = [string]$all[$names[0]]
            foreach ($n in $names) { [System.Environment]::SetEnvironmentVariable($n, $null, 'Process') }
            [System.Environment]::SetEnvironmentVariable($names[0], $val, 'Process')
        }
    }

    $projRoot = "$PSScriptRoot\build"
    $oldCmPath = "C:\Program Files\Python311\Lib\site-packages\cmake\data"
    $newCmPath = "C:\Users\jy\.workbuddy\binaries\python\envs\default\Lib\site-packages\cmake\data"
    foreach ($f in (Get-ChildItem -Path $projRoot -File | Where-Object { $_.Extension -in @('.vcxproj', '.props', '.targets') })) {
        $c = Get-Content $f.FullName -Raw
        if ([string]::IsNullOrEmpty($c)) { continue }
        $orig = $c
        $c = [regex]::Replace($c, '(?s)<Command([^>]*)>[^<]*--check-stamp-file[^<]*</Command>', '<Command$1>exit /b 0</Command>')
        $c = $c.Replace($oldCmPath, $newCmPath)
        $c = $c.Replace('cmake-4.4', 'cmake-3.31')
        if ($c -ne $orig) { Set-Content -Path $f.FullName -Value $c -NoNewline -Encoding UTF8 }
    }

    foreach ($p in @("core_lib", "client_lib", "teacher_gui", "client_gui", "teacher_cli",
                     "client_console", "diag_dirty", "diag_decode", "diag_fec")) {
        Write-Host "  msbuild $p ..." -ForegroundColor DarkGray
        # /t:Rebuild on purpose: MSBuild's incremental file tracker has been observed to MISS a source
        # change on this machine (a .obj stayed 120s older than its .cpp and the "successful" build
        # shipped stale code). A release package must never depend on that tracker -- correctness of
        # the shipped binaries beats build time. (The daily dev loop uses the incremental recipe.)
        & $msbuild "$projRoot\$p.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 `
            /p:WindowsTargetPlatformVersion=10.0.22621.0 /p:BuildProjectReferences=false `
            /m /nologo /v:m 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { Write-Error "编译失败: $p (exit=$LASTEXITCODE)"; exit 1 }
    }
    Write-Host "编译完成。" -ForegroundColor Green
}

# ---------- 2. 收集 payload(两端 exe + teacher_cli + FFmpeg DLL; 供 zip 与 MSI 校验) ----------
$dlls = @()
foreach ($p in @("avcodec-", "avutil-", "swscale-", "swresample-")) {
    $dlls += Get-ChildItem $BuildDir -Filter "$p*.dll" | Select-Object -ExpandProperty FullName
}
$payload = @(
    (Join-Path $BuildDir "client_gui.exe"),
    (Join-Path $BuildDir "teacher_gui.exe"),
    (Join-Path $BuildDir "teacher_cli.exe")   # 命令行教师端(脚本/运维场景), 进入解压版 zip; MSI 按 Product.wxs 只装 GUI 端
) + $dlls
foreach ($f in $payload) {
    if (-not (Test-Path $f)) { Write-Error "缺少 payload 文件: $f"; exit 1 }
}

# ---------- 3. WiX 生成 MSI ----------
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
$wixDir = Join-Path $PSScriptRoot "third_party\wix"
$candle = Join-Path $wixDir "candle.exe"
$light  = Join-Path $wixDir "light.exe"
if (-not (Test-Path $candle) -or -not (Test-Path $light)) {
    Write-Error "未找到 WiX 工具链, 请确认 third_party\wix 完整: $wixDir"; exit 1
}
$wxs    = Join-Path $PSScriptRoot "msi\Product.wxs"
$wxl    = Join-Path $PSScriptRoot "msi\zh-CN.wxl"
$wixobj = Join-Path $PSScriptRoot "msi\Product.wixobj"
$msiOut = Join-Path $DistDir "ScreenCast_v${version}.msi"

Write-Host "candle: 编译 WiX 源文件 msi\Product.wxs ..." -ForegroundColor Cyan
& $candle -nologo -arch x64 "-dVersion=$msiVersion" $wxs -out $wixobj 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Error "candle 编译失败(exit=$LASTEXITCODE)"; exit 1 }

Write-Host "light:  链接生成 MSI ..." -ForegroundColor Cyan
& $light -nologo -ext WixUIExtension -cultures:zh-CN -loc $wxl -b $BuildDir -out $msiOut $wixobj 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Error "light 链接失败(exit=$LASTEXITCODE)"; exit 1 }

# 清理 light 生成的调试符号文件(.wixpdb)
Get-ChildItem $DistDir -Filter "*.wixpdb" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$msiMB = [math]::Round((Get-Item $msiOut).Length/1MB,1)
Write-Host "MSI 安装包: $msiOut ($msiMB MB)" -ForegroundColor Green

# ---------- 4. 顺带打包 zip(解压版) + 记录版本 ----------
$zipDir = Join-Path $DistDir "ScreenCast_v${version}_解压版"
if (Test-Path $zipDir) { Remove-Item $zipDir -Recurse -Force }
New-Item -ItemType Directory -Path $zipDir -Force | Out-Null
Copy-Item -Path $payload -Destination $zipDir

# Qt6 运行时 + 插件 + VC++ 运行库(解压版必须携带, 否则目标机无 Qt/VC redist 时报"Qt 平台无法初始化")
$qtDlls = Get-ChildItem $BuildDir -Filter "Qt6*.dll" | Select-Object -ExpandProperty FullName
$vcDlls = @("d3dcompiler_47.dll","msvcp140.dll","msvcp140_1.dll","msvcp140_2.dll",
            "msvcp140_atomic_wait.dll","msvcp140_codecvt_ids.dll","vcruntime140.dll",
            "vcruntime140_1.dll","vcruntime140_threads.dll") |
          ForEach-Object { Join-Path $BuildDir $_ }
Copy-Item -Path ($qtDlls + $vcDlls) -Destination $zipDir -Force -ErrorAction SilentlyContinue
foreach ($sub in @("platforms","styles","tls","imageformats","iconengines","networkinformation","generic","translations")) {
    $src = Join-Path $BuildDir $sub
    if (Test-Path $src) { Copy-Item -Path $src -Destination $zipDir -Recurse -Force }
}
Copy-Item -Path (Join-Path $BuildDir "LGPL-3.0.txt") -Destination $zipDir -Force -ErrorAction SilentlyContinue
$zipFile = Join-Path $DistDir "ScreenCast_v${version}_解压版.zip"
if (Test-Path $zipFile) { Remove-Item $zipFile -Force }
$seven = "C:\Program Files\7-Zip\7z.exe"
if (Test-Path $seven) {
    & $seven a -tzip -y $zipFile (Join-Path $zipDir "*") | Out-Null
    $zipMB = [math]::Round((Get-Item $zipFile).Length/1MB,1)
    Write-Host "解压版 zip: $zipFile ($zipMB MB)" -ForegroundColor Green
} else {
    Compress-Archive -Path (Join-Path $zipDir "*") -DestinationPath $zipFile -Force
    $zipMB = [math]::Round((Get-Item $zipFile).Length/1MB,1)
    Write-Host "解压版 zip: $zipFile ($zipMB MB)" -ForegroundColor Green
}
Remove-Item $zipDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "====================" -ForegroundColor Cyan
Write-Host "打包完成! 版本 v$version" -ForegroundColor Green
Write-Host "MSI 安装包:  dist\ScreenCast_v${version}.msi" -ForegroundColor Green
Write-Host "解压版 zip:  dist\ScreenCast_v${version}_解压版.zip" -ForegroundColor Green
Write-Host "双击 MSI 安装: 可选择 学生端/教师端, 自动创建桌面快捷方式, 支持在「设置->应用」中卸载。" -ForegroundColor Green
Write-Host "====================" -ForegroundColor Cyan
