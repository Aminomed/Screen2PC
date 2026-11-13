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
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $SetupExe --uninstall
    $ErrorActionPreference = $prevEap
} else {
    Write-Warning "Screen2PC.DriverSetup.exe nicht gefunden."
}

$OldInfs = Get-ChildItem -Path "$env:WINDIR\INF" -Filter "oem*.inf" -ErrorAction SilentlyContinue | Where-Object {
    $raw = Get-Content -Path $_.FullName -Raw -ErrorAction SilentlyContinue
    $raw -and ($raw -match "Root\\Screen2PCVDD" -or $raw -match "Screen2PCDriver\.dll")
}
foreach ($OldInf in $OldInfs) {
    Write-Host "Entferne Treiberpaket $($OldInf.Name) aus DriverStore..." -ForegroundColor DarkGray
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & pnputil.exe /delete-driver $OldInf.Name /uninstall /force
    $ErrorActionPreference = $prevEap
}

if (Test-Path "C:\ProgramData\Screen2PC\monitors.cfg") {
    Remove-Item "C:\ProgramData\Screen2PC\monitors.cfg" -Force -ErrorAction SilentlyContinue
}

Write-Host "Screen2PC Virtual Display Treiber wurde deinstalliert." -ForegroundColor Green
