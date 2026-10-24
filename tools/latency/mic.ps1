# Builds and runs the microphone latency tool (MSVC). Run it once per setup with a different label
# (cable, bluetooth, bridge); the report lands in %TEMP%\magic-keyboard-mic-<label>.txt.
#   mic.ps1 -List                     capture devices
#   mic.ps1 -Label cable [-Mic USB]    measure, optionally picking the mic by part of its name
param(
    [string]$Label,
    [int]$Presses = 30,
    [string]$Mic = '',
    [switch]$List
)
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $here 'mic_latency.cpp'
$build  = Join-Path $env:TEMP 'magic-keyboard-mic-latency'
$exe    = Join-Path $build 'mic_latency.exe'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
# vcvars64.bat calls vswhere itself and expects it on PATH
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$vs      = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars  = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

New-Item -ItemType Directory -Force $build | Out-Null
cmd /c "`"$vcvars`" >nul && cl /nologo /std:c++latest /O2 /EHsc /W4 /utf-8 /Fo`"$build\\`" /Fe`"$exe`" `"$source`""
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($List) {
    & $exe --list
    exit 0
}
if (-not $Label) {
    Write-Error 'Pass -Label (cable, bluetooth, bridge...) or -List'
}

& $exe $Label $Presses (Join-Path $env:TEMP "magic-keyboard-mic-$Label.txt") $Mic
