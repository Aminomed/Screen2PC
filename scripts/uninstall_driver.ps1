#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Entfernt den Screen2PC Virtual Display Treiber sauber vom Surface.
#>
$ErrorActionPreference = "Stop"

$RootDir = $PSScriptRoot
if (-not (Test-Path (Join-Path $RootDir "Screen2PC.DriverSetup.exe"))) {
    $RootDir = Split-Path -Parent $PSScriptRoot
}
$SetupExe = Join-Path $RootDir "Screen2PC.DriverSetup.exe"

if (Test-Path $SetupExe) {
    Write-Host "Entferne Root\Screen2PCVDD Geraet und Treiberpaket..." -ForegroundColor Cyan
    & $SetupExe --uninstall
} else {
    Write-Warning "Screen2PC.DriverSetup.exe nicht gefunden."
}

if (Test-Path "C:\ProgramData\Screen2PC\monitors.cfg") {
    Remove-Item "C:\ProgramData\Screen2PC\monitors.cfg" -Force -ErrorAction SilentlyContinue
}

Write-Host "Screen2PC Virtual Display Treiber wurde deinstalliert." -ForegroundColor Green
