$ErrorActionPreference = 'Stop'
$plannerRoot = Split-Path -Parent $PSScriptRoot
$plannerPython = Join-Path $plannerRoot 'runtime\python.exe'
if (-not (Test-Path -LiteralPath $plannerPython)) {
    throw 'Run this helper from the complete Keqi portable folder.'
}
$plannerAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $plannerAdmin) {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"')) -Wait
    exit
}
$plannerRule = 'Keqi-Calendar-LAN-8768'
Get-NetFirewallRule -Name $plannerRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -Name $plannerRule -DisplayName 'Keqi Calendar (local subnet only)' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8768 -RemoteAddress LocalSubnet -Program $plannerPython -Profile Private,Public | Out-Null
