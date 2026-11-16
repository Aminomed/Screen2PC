# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `PC Task-Manager Autostart, Treiberfreie scRGB-HDR/Gamma-2.2 Farbkalibrierung & GitHub Auto-Updater + Releases`
- **Status:** Alle 3 neuen User-Wünsche sind implementiert und im grünen CI-Build **Run `43840850872`** (Commit `c2ccc12`, Artifact `14213151821`, **GitHub Release `v1.5.0.25`**, 10/10 Unit-Tests PASS, INF/CAT OK, 0 Build-Fehler) veröffentlicht:
  1. **Autostart auf PC & Surface (Task-Manager-kompatibel):** Checkbox *"Mit Windows starten (minimiert im System-Tray, Task-Manager Autostart)"* sowohl im PC-Receiver (`Screen2PC Receiver (PC)`) als auch im Surface-Sender (`Screen2PC Sender (Surface)`) über `HKCU\...\Run` mit `"..." --tray`, Windows Task-Manager `StartupApproved\Run`-Sync, `VS_VERSION_INFO` und separaten Single-Instance-Mutexes (`Screen2PC_Receiver_SingleInstance` / `Screen2PC_Sender_SingleInstance`). Auf dem Surface startet der Sender im Tray ohne virtuelle Monitore vorab anzulegen und wartet auf `Ctrl+Shift+K` oder Tray-Klick.
  2. **HDR & Pro-Monitor Farbkalibrierung (100 % treiberfrei auf PC):** `DXGI_FORMAT_R16G16B16A16_FLOAT` `scRGB` (`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) auf HDR-Displays + reines Gamma `2.2` EOTF + `DISPLAYCONFIG_SDR_WHITE_LEVEL` Nits-Abgleich (inkl. Cursor-White-Level-Parität), `BT.709`-Colorimetry in `CodecH264` sowie Live-Farbfenster pro Monitor (`receiver.ini` `[ColorMonitor<N>]`).
  3. **GitHub Auto-Updater & Releases (PC & Surface):** CI erstellt bei jedem Push auf `main` ein öffentliches GitHub Release (`v1.5.0.<RunNumber>`, `Screen2PC-Windows-x64.zip`). Sender & Receiver prüfen via WinHTTP unauthentifiziert auf `/repos/Aminomed/Screen2PC/releases/latest`, benachrichtigen bei neuerer Version und aktualisieren atomar per PowerShell-Updater (PC treiberfrei; Surface prüft `Screen2PCDriver.dll`/`.inf` SHA256 und installiert den Treiber nur bei Treiber-Änderung + aktivem Treiber per UAC nach).
- **Compiler-, INF- & Test-Realität:**
  - *Build & INF:* Windows-x64 Release-Build, `infverif.exe /v`, `Inf2Cat.exe`, GitHub Release Publish & `Screen2PC.Tests.exe` laufen via `.github/workflows/ci.yml` (`windows-2022`).
  - *Test-Scope:* `Screen2PC.Tests.exe` prüft 10/10 Suiten inkl. `TestAutostartAndCli`, `TestHdrColorPipeline` und `TestUpdaterParsing`; reale Registry/Task-Manager-UI, `IDXGIOutput6`/`scRGB`-SwapChain, `WM_INPUT`/`ClipCursor`, `DxgiCapture`, UMDF2-Treiber & echter HTTPS-Update-Download laufen nur auf realer Windows-Hardware.
- **Next Step:** Aktuelles GitHub Release auf **PC** und **Surface** testen (Autostart auf beiden Geräten, HDR- vs. SDR-Farben + Feintuning, sowie 1-Klick-Updater ab `v1.5.0.25+`).

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440` (HDR), Monitor 2: `1920x1080` (SDR).
- **Kernfunktionen:**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im randlosen Vollbild (`WS_POPUP`) oder Fenstermodus (`WS_OVERLAPPEDWINDOW`) inkl. `scRGB`-HDR- & SDR-Farbanpassung dargestellt.
  - Automatischer Windows-x64-Release-Build + Tests + INF/CAT-Prüfung + GitHub Release (`Screen2PC-Windows-x64.zip`) via GitHub Actions; integrierter 1-Klick Auto-Updater auf PC & Surface.
  - Bidirektionale Zwischenablage (Text/Bilder), Full-Duplex Quick File Transfer (PC <-> Surface) und nahtloses Full-Desktop-KVM (PC-Maus/Tastatur steuert virtuelle PC-Displays **und** physischen Surface-Screen ohne PC-Treiber/Hooks).

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (`stdcpp20`, `x64`-only, `/MT` Release / `/MTd` Debug, VS 2022 `v143`)**
   - *Stack:* Win32 + D3D11 + DXGI 1.6 (`IDXGIOutput6`) + Media Foundation + Winsock2 + WinHTTP + `winmm` (`timeBeginPeriod(1)`) + UMDF v2 (`2.15`, `IddCx 1.2`), `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`.
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, schont RAM auf dem 8-GB-Surface, verhindert GC-Mikroruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen `Screen2PC.sln`.
2. **PC bleibt komplett treiberfrei (Pure User-Mode KVM mit Full-Desktop Roaming + Task-Manager Autostart):**
   - `Screen2PC.Receiver.exe` installiert keinen Treiber und nutzt kein `SetWindowsHookEx`.
   - **Autostart (`Autostart.h/.cpp`):** Trägt `"Screen2PC Receiver"` unter `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` mit `"<exe>" --tray` ein und hält `StartupApproved\Run` synchron. Im Windows Task-Manager erscheint der Eintrag dank `.rc` `VS_VERSION_INFO` sauber als **`Screen2PC Receiver (PC)`** unter **Autostart-Apps** und lässt sich dort direkt aktivieren/deaktivieren (`WM_ACTIVATE` synchronisiert die Checkbox bei Rückkehr ins Fenster). Ein Single-Instance-Mutex (`Local\Screen2PC_Receiver_SingleInstance`) sorgt dafür, dass ein späterer Start ohne `--tray` das bereits im Tray laufende Fenster per `WM_SHOW_EXISTING` in den Vordergrund holt.
   - **`D3D11Renderer` KVM-Capture (`RegisterRawInputDevices` mit `dwFlags = 0, hwndTarget = nullptr` + `ClipCursor` + `SetCursor(nullptr)`):**
     - Im aktivierten KVM-Modus wird bei Klick ins Bild (oder Vollbild-Fokus) einmalig die Startposition ans Surface gewarpt (`isRelative = 0`) und danach werden ungedrosselte HID-Relativdeltas (`WM_INPUT` `RIM_TYPEMOUSE`, `isRelative = 1` in `KvmMouseEvent::pad[0]`, koalesziert pro Render-/Pump-Tick) ans Surface gesendet, während der lokale PC-Cursor per `ClipCursor` im Client-Rechteck gehalten und per `SetCursor(nullptr)` unsichtbar geschaltet ist (kein „Doppel-Cursor“ / Nachziehen!).
     - Auf dem Surface rechnet `KvmInput::StepVirtualDesktopPoint` die Pixel-Deltas über alle physischen + virtuellen Monitore (`EnumDisplayMonitors`, 1s-Cache) inkl. nahtlosem Border-Snapping bei ungleichen Höhen (`1504` / `1440` / `1080`). Damit wandert der Surface-Mauszeiger frei zwischen Display 1, Display 2 und dem **physischen Surface-Bildschirm** (inkl. Drag & Drop von Fenstern!).
     - `ESC` (verlässt Vollbild + löst Capture), `Ctrl+Alt+K` (synchronisiert `D3D11Renderer`, `ReceiverEngine::m_kvmEnabled` und Checkbox via `KvmToggleCallback`), `WM_ACTIVATE(WA_INACTIVE)` und `WM_KILLFOCUS` geben `ClipCursor` und lokalen Cursor sofort wieder frei.
3. **HDR- & Pro-Monitor-Farbanpassung (`D3D11Renderer`, `ColorProfile.h/.cpp`, `CodecH264.cpp` — 100 % treiberfrei):**
   - *Transport-Colorimetry:* `Protocol.h` bleibt unverändert; Video läuft weiter in 8-Bit SDR, aber `CodecH264` setzt explizit `MF_MT_YUV_MATRIX = BT709`, `MF_MT_VIDEO_PRIMARIES = BT709`, `MF_MT_TRANSFER_FUNCTION = 709` (inkl. Strip-and-Retry-Fallback bei MFT-Ablehnung), `ConfigureVideoProcessorBt709` und Studio-Range-BT.709 CPU-Fallback-Koeffizienten (`/4096`).
   - *HDR `scRGB`-Output auf PC (`D3D11Renderer` + `ColorProfile`):* Erkennt pro Zielmonitor via `IDXGIOutput6::GetDesc1()` (`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`) + `QueryDisplayConfig` / `DISPLAYCONFIG_SDR_WHITE_LEVEL` automatisch den HDR-Zustand und Windows-SDR-Weißpegel (`osSdrWhiteScale`, z. B. `240 nits = 3.0x`). Auf HDR-Displays wird die SwapChain in `DXGI_FORMAT_R16G16B16A16_FLOAT` (`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) erstellt; der Pixel-Shader (`PSMain` + `ColorCB register(b1)`) wendet Kontrast, reine Gamma-`2.2`-EOTF (`pow(rgb, 2.2)` statt sRGB-Transferkurve -> keine aufgehellten Schatten), BT.709-Sättigung, RGB-Gain sowie `osSdrWhiteScale * brightness` an (inkl. Cursor-Overlay mit erzwungener Neutralität für Kontrast/Sättigung).
   - *Live-Kalibrierungsfenster:* Im PC-Receiver über *"Farben & HDR pro Monitor anpassen..."* (`receiver.ini` `[ColorMonitor<N>]`) für Helligkeit (`50–200 %`), Gamma (`1.60–2.80`), Kontrast (`60–140 %`), Sättigung (`50–160 %`), RGB-Gains (`70–130 %`) und scRGB-HDR-Schalter live auf laufende Streams anwendbar; `WM_DISPLAYCHANGE`/`WM_MOVE` bauen SwapChain bei Monitor-/HDR-Wechsel automatisch neu.
4. **GitHub Releases & Auto-Updater (`Version.h`, `Directory.Build.props`, `Updater.h/.cpp`, `ci.yml`):**
   - `Version.h` liefert `1.5.0.<SCREEN2PC_BUILD_NUMBER>` (aus `GITHUB_RUN_NUMBER` in `Directory.Build.props`, fallback `0` lokal), eingebettet in C++ & Win32 `.rc` `VS_VERSION_INFO`.
   - `ci.yml` veröffentlicht bei Push auf `main` nach grünem Build, Tests & `infverif`/`Inf2Cat` automatisch das Release `v1.5.0.<RunNumber>` (`--latest`) mit `Screen2PC-Windows-x64.zip`.
   - `Updater.cpp` nutzt WinHTTP HTTPS gegen `https://api.github.com/repos/Aminomed/Screen2PC/releases/latest` (ohne API-Token, `JsonCursor` tiefenbewusst), lädt das ZIP bei Bestätigung nach `%TEMP%\Screen2PC_Update_<pid>\`, prüft ZIP-Magic `PK\x03\x04` und Größe, und startet ein PowerShell-Update-Skript (wartet auf PID-Exit, entpackt, kopiert Dateien relativ inkl. `driver\`, entfernt MOTW via `Unblock-File`, prüft am Sender SHA256 von `Screen2PCDriver.dll`/`.inf` und triggert UAC-Treiberupdate nur bei Änderung + installiertem Treiber, und startet Sender/Receiver inkl. `--tray` neu).
5. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx 1.2`):**
   - Minimaler User-Mode-Display-Treiber ohne eigenes Kernel-`.sys` (`Screen2PCDriver.inf` mit `ClassVer = 2.0`, `HardwareID = Root\Screen2PCVDD`, `PnpLockdown = 1`, `MaxMonitorsSupported = 2`, `UmdfExtensions = IddCx0102` in `[Screen2PCDriver_Install_UMDF]`, ohne `ProcessSharingDisabled`-Konflikt; Win10 `17763` über `AddService = WUDFRd` und Win11 `22000+` über `Include/Needs = WUDFRD.inf`).
   - *IPC & Persistenz:* Named Pipe `\\.\pipe\Screen2PC_VDD`, Persistenz unter `C:\ProgramData\Screen2PC\monitors.cfg`.
6. **Dual-Codec & Low-Latency Pipeline + Transport (`Protocol.h`, `NetSocket`):**
   - H.264 Low-Latency (`25 / 50 / 80 / 120 / 200 Mbit/s`, `timeBeginPeriod(1)`, Async-MFT-Wait-Fix, kein Post-Frame-`Sleep`) oder `DeltaLossless` (`DLT1`).
   - IPv4-only (`AF_INET`), `S2P1` Magic, Version `1`, **ohne TLS/Auth**, `Protocol.h`-Binärstructs unverändert. `ClipboardSync` / `FileTransfer` mit `m_lifecycleMutex` (Move-before-Join).

---

## 3. Repository Map
| Pfad | Inhalt & Zweck |
|---|---|
| `MEMORY.md`, `README.md` | Zentrale Projekt-Memory (`MEMORY.md`) & Enduser-Schnellstart (`README.md`) |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props`, `.gitignore` | VS 2022 Solution (`x64`, `SCREEN2PC_BUILD_NUMBER`), NuGet-WDK/-SDK `10.0.26100.2454` für Driver |
| `src/common/` (`Screen2PC.Common.vcxproj`) | `Protocol.h`, `Version.h`, `Autostart.h/.cpp`, `ColorProfile.h/.cpp`, `Updater.h/.cpp`, `Logger.h/.cpp`, `NetSocket.h/.cpp`, `CodecH264.h/.cpp` (`BT.709`), `CodecDelta.h/.cpp`, `ClipboardSync.h/.cpp`, `FileTransfer.h/.cpp`, `KvmInput.h/.cpp` |
| `src/driver/` (`Screen2PC.Driver.vcxproj`) | `DriverProtocol.h`, `Driver.h/.cpp` (UMDF v2 `IddCx 1.2`), `Screen2PCDriver.inf` |
| `src/driversetup/` (`Screen2PC.DriverSetup.vcxproj`) | `main.cpp` -> `Screen2PC.DriverSetup.exe` |
| `src/sender/` (`Screen2PC.Sender.vcxproj`) | Surface-App: `SenderEngine.h/.cpp`, `DxgiCapture.h/.cpp`, `VirtualDisplayCtl.h/.cpp`, `Screen2PC.Sender.rc`, `main.cpp` (`winmm`, `25..200 Mbit/s`, Task-Manager Autostart `--tray`, Single-Instance, GitHub Auto-Updater + SHA256-Treibercheck) |
| `src/receiver/` (`Screen2PC.Receiver.vcxproj`) | PC-App (treiberfrei): `ReceiverEngine.h/.cpp`, `D3D11Renderer.h/.cpp` (`IDXGIOutput6`/`scRGB`/Gamma `2.2`/`ColorCB`, `WM_INPUT`, `ClipCursor`), `Screen2PC.Receiver.rc`, `main.cpp` (Task-Manager Autostart, `--tray`, Single-Instance, Farbkalibrierungs-Dialog, GitHub Auto-Updater) |
| `src/tests/` (`Screen2PC.Tests.vcxproj`) | `main.cpp` -> `Screen2PC.Tests.exe`: 10 Suiten (Protocol, Delta, Color/H264/BT.709/Version, Loopback, Clipboard, FileTransfer, KVM-Roaming, Autostart/CLI, HDR-Color-Pipeline/INI, Updater-JSON/Version) |
| `scripts/` | `install_driver.ps1`, `uninstall_driver.ps1` |
| `.github/workflows/ci.yml` | Windows-2022 CI: MSBuild `Release\|x64`, Tests, `infverif`, `Inf2Cat`, Artifact & GitHub Release `Screen2PC-Windows-x64.zip` |

---

## 4. Konfiguration, Hotkeys, Firewall & Pfade
- **Sender Display-Modi (`DisplayMode`):** `ExtendAll` (`0`), `ExtendPrimary` (`1`), `MirrorPrimary` (`2`), `ExtendSecondary` (`3`).
- **Hotkeys & Bedienung:**
  - *Sender (`530x626`):* Globaler Hotkey `Ctrl+Shift+K` (Verbinden/Trennen); `X` minimiert ins Tray; Checkbox *"Mit Windows starten (minimiert im System-Tray, Task-Manager Autostart)"*; Auto-Update-Checkbox + *"Nach Updates suchen"* + Tray-Menüpunkt.
  - *Receiver (`580x550`):* `X` versteckt ins Tray; Button *"Windows Firewall Ports freigeben (Admin)"*; Checkbox *"Mit Windows starten (minimiert im System-Tray, Task-Manager Autostart)"*; Button *"Farben & HDR pro Monitor anpassen..."*; Auto-Update-Checkbox + *"Nach Updates suchen"* + Tray-Menüpunkt.
  - *Streamfenster (`D3D11Renderer`):* `F11` Vollbild-Toggle; `ESC` verlässt Vollbild **und** gibt KVM-Mausfokus frei; Klick ins Bild aktiviert Full-Desktop-KVM-Capture (`WM_INPUT` + `ClipCursor`); `Ctrl+Alt+K` schaltet KVM global um; `Alt+F4` beendet Session (`StopSession`); `WM_DISPLAYCHANGE`/`WM_MOVE` aktualisieren HDR/SDR-SwapChain automatisch.
- **INI-Konfiguration, Registry & Pfade:**
  - `%APPDATA%\Screen2PC\sender.ini` (`[Sender]`): `LastIp=""`, `ModeIdx=0`, `CodecIdx=0`, `BitrateIdx=2`, `ClipboardSync=1`, `KvmEnabled=1`, `AutoUpdateCheck=1`.
  - `%APPDATA%\Screen2PC\receiver.ini` (`[Receiver]`): `StartFullscreen=1`, `ClipboardSync=1`, `KvmEnabled=1`, `AutoUpdateCheck=1`; `[ColorMonitor0]`, `[ColorMonitor1]`: `HdrOutput=1`, `BrightnessPct=100`, `GammaX100=220`, `ContrastPct=100`, `SaturationPct=100`, `RedGainPct=100`, `GreenGainPct=100`, `BlueGainPct=100`.
  - Registry (je optional per Checkbox): `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Screen2PC Receiver` / `Screen2PC Sender` & `StartupApproved\Run`.
  - `C:\ProgramData\Screen2PC\monitors.cfg`, `%USERPROFILE%\Downloads\Screen2PC`, `%TEMP%\Screen2PC_*.log`, `%TEMP%\Screen2PC_Update_<pid>\`.

---

## 5. Bekannte technische Eigenheiten & Grenzen
1. **Receiver-Session-Limits:** Receiver akzeptiert nur **1** aktive Session gleichzeitig; `DiscoverResp`-Portfelder werden vom Sender beim Connect ignoriert (`48211` / `48212+i` fest).
2. **Input/UIPI & Hotkey-Details:** `SendInput` am Surface unterliegt Windows-UIPI (ohne Admin-Start keine Steuerung von UAC-/Admin-Fenstern); beim Verlassen via `ESC` wird `m_suppressEscUp` lokal abgefangen. `F11` prüft Repeat-Bit; `Ctrl+Shift+K` (`RegisterHotKey`) könnte künftig `MOD_NOREPEAT` nutzen.
3. **Codec-/Clipboard-/Socket-Grenzen:** Runtime-H.264-Fallback auf Sender (`GPU->CPU->Delta`) und Receiver (`DXVA->CPU`) greift nur vor dem ersten erfolgreich dekodierten Frame; `ClipboardSync` kappt Text bei `2 MB` (`CF_DIB` bis `16 MB`); `UdpSocket::m_sock` ist kein `std::atomic<SOCKET>`.
4. **Updater- & HDR-Hinweis:** Das allererste Release (`Screen2PC-Windows-x64.zip`) muss noch manuell von GitHub Releases heruntergeladen und entpackt werden, da erst ab `v1.5.0.25` der integrierte Auto-Updater enthalten ist; bei künftigen Versionen reicht der 1-Klick-Dialog. Auf dem PC kann je nach Monitor-Panel die Feinkalibrierung (z. B. Brightness/Gamma/RGB-Gain) einmalig im *"Farben & HDR pro Monitor anpassen..."*-Fenster dem SDR-Zweitmonitor angeglichen werden.

---

## 6. Changelog
| Datum | Schritt | Änderung |
|---|---|---|
| 2026-11-12 | Init & Planung | `MEMORY.md` angelegt; Hardware-Specs, C++20-Entscheidung, Dual-Codec & treiberfreier PC-Empfänger fixiert |
| 2026-11-12 | Initial Implementation | `Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Solution, Scripts, `ci.yml` & `README.md` erstellt (`a8543e7`) |
| 2026-11-12 | CI, Driver & Codec Fixes | WDK NuGet `10.0.26100.2454` + MSBuild in `ci.yml`; `H264Decoder` Crop-Fix; Test-Suites (`5d07e38`..`43e9140`) |
| 2026-11-13 | Phase 1–3 & Hardening | `ClipboardSync` (`0bea31f`), `FileTransfer` (`0328efb`), KVM (`ffa80dc`), Concurrency-/Fallback-Hardening (`f205ed4`..`87f0a7e`) |
| 2026-11-13 | INF Problem-31 Fix (Hardware OK) | `UmdfExtensions = IddCx0102`, Win11 `WUDFRD.inf`, `.cat`-only Signierung & `oem*.inf`-Purge (`f92af3f`, Run `43422111904`) — auf Surface + PC mit 2 Monitoren erfolgreich verifiziert |
| 2026-11-13 | Full-Desktop KVM, Disconnect-Fix & High-FPS/Bitrate | `WM_INPUT` + `ClipCursor` + `SetCursor(nullptr)` + `StepVirtualDesktopPoint` (`65dfd15`); Disconnect-Crash-Fix (`ClipboardSync`/`FileTransfer`/`SenderEngine`); 30-Hz-Sleep-Bug & Async-MFT-Stall behoben + `timeBeginPeriod(1)` + `25..200 Mbit/s` (`Run 43435633945`) |
| 2026-11-16 | PC Autostart, scRGB HDR & GitHub Auto-Updater | `Autostart` (`HKCU\Run` + `StartupApproved\Run` Task-Manager-Sync + `--tray` + Single-Instance-Mutex + `.rc` `VERSIONINFO`); `scRGB` (`R16G16B16A16_FLOAT`) HDR + Gamma `2.2` + `DISPLAYCONFIG_SDR_WHITE_LEVEL` + `BT.709` + Live-Farbkalibrierungsfenster; `Updater` (WinHTTP GitHub Release Check & atomares PowerShell Self-Update + CI Release Publishing); 10/10 Unit-Tests (`c2ccc12`, Run `43840850872`, Artifact `14213151821`, Release `v1.5.0.25`) |
| 2026-11-16 | Surface Sender Autostart (Variante A) | Gleicher Task-Manager-Autostart (`Screen2PC Sender`, `--tray`, `Local\Screen2PC_Sender_SingleInstance`, `WM_ACTIVATE`-Sync) auch in `src/sender/main.cpp` ergänzt |
