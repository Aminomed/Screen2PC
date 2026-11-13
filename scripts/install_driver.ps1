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

Write-Host "[1/5] Entferne ggf. Download-Blockierung (SmartScreen Zone.Identifier)..." -ForegroundColor Cyan
Get-ChildItem -Path $RootDir -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

Write-Host "[2/5] Bereinige alte Screen2PC-Geraete und zwischengespeicherte oem*.inf Pakete..." -ForegroundColor Cyan
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $SetupExe --uninstall > $null 2>&1
$ErrorActionPreference = $prevEap

$OldInfs = Get-ChildItem -Path "$env:WINDIR\INF" -Filter "oem*.inf" -ErrorAction SilentlyContinue | Where-Object {
    $raw = Get-Content -Path $_.FullName -Raw -ErrorAction SilentlyContinue
    $raw -and ($raw -match "Root\\Screen2PCVDD" -or $raw -match "Screen2PCDriver\.dll")
}
foreach ($OldInf in $OldInfs) {
    Write-Host "  Entferne altes Treiberpaket $($OldInf.Name)..." -ForegroundColor DarkGray
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & pnputil.exe /delete-driver $OldInf.Name /uninstall /force > $null 2>&1
    $ErrorActionPreference = $prevEap
}

Write-Host "[3/5] Erstelle/Pruefe Zertifikat und signiere Katalog (.cat)..." -ForegroundColor Cyan
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

$PkgDir = Split-Path -Parent $InfPath
$CatFiles = Get-ChildItem -Path $PkgDir -Filter "*.cat" -ErrorAction SilentlyContinue
if (-not $CatFiles) {
    Write-Warning "Keine .cat-Datei gefunden in $PkgDir (Katalog-Signatur uebersprungen)."
}
foreach ($CatFile in $CatFiles) {
    $SigCat = Set-AuthenticodeSignature -FilePath $CatFile.FullName -Certificate $Cert -HashAlgorithm SHA256
    if ($SigCat.Status -ne "Valid") {
        Write-Warning "CAT Signatur-Status ($($CatFile.Name)): $($SigCat.Status) ($($SigCat.StatusMessage))"
    }
}

Write-Host "[4/5] Richte C:\ProgramData\Screen2PC Verzeichnis ein..." -ForegroundColor Cyan
$CfgDir = "C:\ProgramData\Screen2PC"
New-Item -ItemType Directory -Path $CfgDir -Force | Out-Null
icacls $CfgDir /grant "*S-1-5-11:(OI)(CI)M" /grant "*S-1-5-19:(OI)(CI)RX" | Out-Null

Write-Host "[5/5] Installiere Root\Screen2PCVDD via Screen2PC.DriverSetup.exe..." -ForegroundColor Cyan
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $SetupExe --install $InfPath
$Code = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($Code -ne 0) {
    Write-Warning "DriverSetup meldete Exit-Code $Code."
}

$PipeOk = $false
for ($attempt = 1; $attempt -le 12; $attempt++) {
    Start-Sleep -Milliseconds 500
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $SetupExe --ping > $null 2>&1
    $pingProbe = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($pingProbe -eq 0) {
        $PipeOk = $true
        break
    }
}

$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $SetupExe --status
& $SetupExe --ping
$FinalPingCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap

if ($FinalPingCode -ne 0 -or -not $PipeOk) {
    Write-Host ""
    Write-Host "--- Diagnose-Informationen ---" -ForegroundColor Yellow
    try {
        $sb = Confirm-SecureBootUEFI
        Write-Host "Secure Boot: $sb"
    } catch {
        Write-Host "Secure Boot: Unbekannt ($($_.Exception.Message))"
    }
    try {
        $hvci = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" -Name "Enabled" -ErrorAction SilentlyContinue
        Write-Host "Speicher-Integritaet (HVCI): $(if ($hvci) { $hvci.Enabled } else { 'Nicht gesetzt' })"
    } catch {}

    foreach ($logName in @("Microsoft-Windows-Kernel-PnP/Configuration", "Microsoft-Windows-DriverFrameworks-UserMode/Operational")) {
        $evts = Get-WinEvent -LogName $logName -MaxEvents 5 -ErrorAction SilentlyContinue
        if ($evts) {
            Write-Host "Letzte Ereignisse [$logName]:" -ForegroundColor Yellow
            foreach ($e in $evts) {
                $msg = ($e.Message -replace "\r?\n", " ")
                if ($msg.Length -gt 220) { $msg = $msg.Substring(0, 220) + "..." }
                Write-Host "  $($e.TimeCreated): [$($e.Id)] $msg"
            }
        }
    }

    Write-Host ""
    Write-Host "HINWEIS: Falls Windows den selbstsignierten Treiber weiterhin blockiert (z. B. Problem 52 / Code Integrity):" -ForegroundColor Yellow
    Write-Host "  1. Auf dem Surface (nur Surface, NICHT PC!): Bei Bedarf Test-Signing aktivieren (erfordert ggf. deaktiviertes Secure Boot): bcdedit /set testsigning on" -ForegroundColor Yellow
    Write-Host "  2. Oder Screen2PC.Sender.exe im Modus 'Surface-Display spiegeln' komplett treiberfrei nutzen." -ForegroundColor Yellow
} else {
    Write-Host "Fertig! Screen2PC Virtual Display Treiber ist installiert und aktiv." -ForegroundColor Green
}
