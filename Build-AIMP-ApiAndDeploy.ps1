[CmdletBinding()]
param(
    [string]$CMakeExe = '',
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$OutputDirectory = 'artifacts/alien-api',
    [string]$SecondPcApi = '\\DESKTOP-2JNROA0\win-unpacked\resources\api',
    [switch]$DeliveryOnly
)
# Compatibility entry point. Keep one canonical build/deploy implementation.
# Forwards only explicitly supplied arguments; canonical script owns defaults.
& (Join-Path $PSScriptRoot 'build-alien-api.ps1') @PSBoundParameters
