# Build TrayMirror spikes. Spec 12.8 — MSVC only.
# Usage:
#   scripts\build.ps1                 # x64 -> build\
#   scripts\build.ps1 -Arch win32     # x86 -> build-x86\ (WOW64 spikes)
#   scripts\build.ps1 -Config Release
param(
    [ValidateSet('x64', 'win32')]
    [string]$Arch = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'

$BuildTools = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$CmakeBin   = Join-Path $BuildTools 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$NinjaBin   = Join-Path $BuildTools 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$Vcvars     = Join-Path $BuildTools 'VC\Auxiliary\Build\vcvarsall.bat'

foreach ($p in @($CmakeBin, $NinjaBin, $Vcvars)) {
    if (-not (Test-Path $p)) { Write-Error "Not found: $p"; exit 1 }
}

if (-not $BuildDir) { $BuildDir = if ($Arch -eq 'x64') { 'build' } else { 'build-x86' } }
$VcArch = if ($Arch -eq 'x64') { 'x64' } else { 'x86' }

$Repo = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $Repo $BuildDir

# CMake and Ninja are not in PATH: prepend them for the duration of the call.
$env:Path = "$CmakeBin;$NinjaBin;$env:Path"

cmd /s /c "`"$Vcvars`" $VcArch && cmake -S `"$Repo`" -B `"$BuildPath`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$BuildPath`""
exit $LASTEXITCODE
