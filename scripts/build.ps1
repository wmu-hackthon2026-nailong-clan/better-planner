# M0 构建脚本：vcvars64 + CMake(NMake) + ctest
# 用法： .\scripts\build.ps1
# VS 不在默认位置时，先设置环境变量 VCVARS64 指向你的 vcvars64.bat。
param()

$ErrorActionPreference = "Stop"

$vcvars = $env:VCVARS64
if (-not $vcvars) { $vcvars = "D:\VisualStudioPackages\VC\Auxiliary\Build\vcvars64.bat" }
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars (set VCVARS64 env var)"
}

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$winkit = $env:WINKITROOT
if (-not $winkit) { $winkit = "C:\Program Files (x86)\Windows Kits\10" }
$winkitver = $env:WINKITVER
if (-not $winkitver) { $winkitver = "10.0.26100.0" }

$bat = Join-Path $buildDir "run-build.bat"
@"
@echo off
call "$vcvars" >nul
set "PATH=$winkit\bin\$winkitver\x64;%PATH%"
set "INCLUDE=$winkit\Include\$winkitver\um;$winkit\Include\$winkitver\ucrt;$winkit\Include\$winkitver\shared;%INCLUDE%"
set "LIB=$winkit\Lib\$winkitver\um\x64;$winkit\Lib\$winkitver\ucrt\x64;%LIB%"
cd /d "$root"
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
ctest --test-dir build --output-on-failure
exit /b %ERRORLEVEL%
"@ | Set-Content -Path $bat -Encoding ascii

& cmd.exe /c "`"$bat`""
exit $LASTEXITCODE
