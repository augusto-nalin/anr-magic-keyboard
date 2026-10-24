# Builds the latency tool, takes the ESP32's serial port back from WSL and runs it.
# WSL gets the port again on the next make target that needs it (make attach).
param(
    [int]$Events = 40
)
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe    = Join-Path $env:TEMP 'magic-keyboard-latency.exe'
$report = Join-Path $env:TEMP 'magic-keyboard-latency.txt'
$csc    = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

& $csc /nologo /optimize /out:$exe (Join-Path $here 'Latency.cs')
if ($LASTEXITCODE -ne 0) { exit 1 }

$usbipd = Join-Path $env:ProgramFiles 'usbipd-win\usbipd.exe'
if (Test-Path $usbipd) {
    # No 2> redirect: Windows PowerShell turns redirected native stderr into a terminating error
    # under 'Stop', and usbipd writes its info lines there
    & $usbipd detach --hardware-id 10c4:ea60
    Start-Sleep -Seconds 2
}

$port = $null
foreach ($device in Get-PnpDevice -PresentOnly -Class Ports) {
    if ($device.FriendlyName -match 'CP210x.*\((COM\d+)\)') {
        $port = $Matches[1]
        break
    }
}
if ($null -eq $port) {
    Write-Error 'No CP210x serial port found, is the ESP32 plugged in?'
}

& $exe $port $Events $report
