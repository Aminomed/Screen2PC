#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Installiert den Screen2PC Virtual Display Treiber (UMDF v2 IddCx) auf dem SURFACE.
  NICHT auf dem Gaming-PC ausfuehren! Der PC-Empfaenger arbeitet 100% treiberfrei.
#>
$ErrorActionPreference = "Stop"

$RootDir = $PSScriptRoot
if (-not (Test-Path (Join-Path $RootDir "Screen2PC.DriverSetup.exe"))) {
    $RootDir = Split-Path -Parent $PSScriptRoot
}
$DriverDir = Join-Path $RootDir "driver"
$InfPath = Join-Path $DriverDir "Screen2PCDriver.inf"
$DllPath = Join-Path $DriverDir "Screen2PCDriver.dll"
$CatPath = Join-Path $DriverDir "Screen2PCDriver.cat"
$SetupExe = Join-Path $RootDir "Screen2PC.DriverSetup.exe"

if (-not (Test-Path $InfPath)) {
    $InfPath = Join-Path $RootDir "Screen2PCDriver.inf"
    $DllPath = Join-Path $RootDir "Screen2PCDriver.dll"
    $CatPath = Join-Path $RootDir "Screen2PCDriver.cat"
}

if (-not (Test-Path $InfPath) -or -not (Test-Path $DllPath) -or -not (Test-Path $SetupExe)) {
    Write-Error "Treiberdateien oder Screen2PC.DriverSetup.exe nicht gefunden in $RootDir"
    exit 1
}

Write-Host "[1/4] Erstelle/Pruefe lokales Code-Signing Zertifikat..." -ForegroundColor Cyan
$Subject = "CN=Screen2PC Local Virtual Display"
$Cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date).AddDays(7) } | Select-Object -First 1
if (-not $Cert) {
    $Cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -NotAfter (Get-Date).AddYears(5)
}

foreach ($StorePath in @("Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher")) {
    $Store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        ($StorePath -replace "Cert:\\LocalMachine\\", ""), "LocalMachine")
    $Store.Open("ReadWrite")
    $Exists = $Store.Certificates | Where-Object { $_.Thumbprint -eq $Cert.Thumbprint }
    if (-not $Exists) {
        $Store.Add($Cert)
    }
    $Store.Close()
}

Write-Host "[2/4] Signiere Treiberpaket lokal (mit Bordmitteln)..." -ForegroundColor Cyan
$SigDll = Set-AuthenticodeSignature -FilePath $DllPath -Certificate $Cert -HashAlgorithm SHA256
if ($SigDll.Status -ne "Valid") {
    Write-Warning "DLL Signatur-Status: $($SigDll.Status) ($($SigDll.StatusMessage))"
}
if (Test-Path $CatPath) {
    $SigCat = Set-AuthenticodeSignature -FilePath $CatPath -Certificate $Cert -HashAlgorithm SHA256
    if ($SigCat.Status -ne "Valid") {
        Write-Warning "CAT Signatur-Status: $($SigCat.Status) ($($SigCat.StatusMessage))"
    }
}

Write-Host "[3/4] Richte C:\ProgramData\Screen2PC Verzeichnis ein..." -ForegroundColor Cyan
$CfgDir = "C:\ProgramData\Screen2PC"
New-Item -ItemType Directory -Path $CfgDir -Force | Out-Null
icacls $CfgDir /grant "*S-1-5-11:(OI)(CI)M" | Out-Null

Write-Host "[4/4] Installiere Root\Screen2PCVDD via Screen2PC.DriverSetup.exe..." -ForegroundColor Cyan
& $SetupExe --install $InfPath
$Code = $LASTEXITCODE
if ($Code -ne 0) {
    Write-Warning "DriverSetup meldete Exit-Code $Code."
}

Start-Sleep -Seconds 2
& $SetupExe --status
& $SetupExe --ping
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "HINWEIS: Falls Windows den selbstsignierten Treiber blockiert (Problem 52 / Code Integrity):" -ForegroundColor Yellow
    Write-Host "  1. Auf dem Surface (nur Surface, NICHT PC!): 'bcdedit /set testsigning on' ausfuehren und neu starten." -ForegroundColor Yellow
    Write-Host "  2. Oder alternativ Screen2PC.Sender.exe im Modus 'Surface-Display spiegeln' ohne Treiber nutzen." -ForegroundColor Yellow
} else {
    Write-Host "Fertig! Screen2PC Virtual Display Treiber ist installiert und aktiv." -ForegroundColor Green
}
