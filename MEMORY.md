# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `Inno Setup Windows-x64-Installer & Autostart-/Shutdown-Integration (Release v1.5.0.57)`
- **Status:** Im grünen CI-Build **Run `45419971886`** (Commit `9179b94`, **GitHub Release `v1.5.0.57`**, **21/21 Test-Suites PASS**, INF/CAT OK) ist zusätzlich zum portablen ZIP (`Screen2PC-Windows-x64.zip`, `1.31 MB`) der geführte Windows-x64-Installer **`Screen2PC-Setup-x64.exe`** (`2.62 MB`) live:
  1. **Inno Setup 6 Installer (`installer/Screen2PC.iss`, `.github/workflows/ci.yml`):**
     - Zweisprachiger moderner Wizard (Deutsch/Englisch), Installation nach `C:\Program Files\Screen2PC` (kompatibel mit dem GitHub Auto-Updater via `needAdmin`), Startmenü- & optionale Desktop-Verknüpfungen sowie Apps-&-Features-Deinstaller.
     - Exklusive Rollenauswahl per `[Types]`/`[Components]`: **Laptop (Sender + UMDF v2 VDD-Treiber)** oder **PC (Empfänger — 100 % treiberfrei + Firewall)** inkl. `[InstallDelete]`-Bereinigung bei Rollenwechsel.
     - Richtet auf Wunsch automatisch den Treiber (`install_driver.ps1` am Laptop) bzw. die Firewall-Ports (`TCP 48211–48215`, `UDP 48210/48215–48217` am PC) sowie den Autostart ein und räumt bei Deinstallation sauber auf (`uninstall_driver.ps1`, Firewall-Regeln, Autostart).
  2. **Headless Autostart-CLI & Graceful Shutdown (`src/sender/main.cpp`, `src/receiver/main.cpp`):**
     - `--install-autostart` und `--uninstall-autostart` erlauben dem Installer, exakt die interne C++20-Autostart-Logik auszuführen (Laptop: Task-XML ohne Akku-Sperre; PC: `HKCU\...\Run` mit `runasoriginaluser`).
     - `WM_QUERYENDSESSION`/`WM_ENDSESSION` schließen Tray-Instanzen bei Upgrades/Windows-Shutdown sauber (`DestroyWindow`), ergänzt um `taskkill`-Fallback im Installer.
  3. **Basis aus `v1.5.0.53`–`v1.5.0.55` weiter aktiv:**
     - Akku-/UIPI-fester Sender-Autostart & `TIMER_TRAY_RETRY` (`v1.5.0.55`), Stream-Timeout-Fix + Auto-Reconnect (`v1.5.0.54`), Direct AMD AMF (`amfrt64.dll` H.264/HEVC), D3D11 Compute-Shader Delta (`GpuDeltaAnalyzer`), `DLT3` QOI-Lossless-Hybrid & UDP Video + GF(2⁸)-Cauchy-RS-FEC (`48216–48217`, Protokoll `v3`).
- **Zurückgestellte Feature-Ideen (Deferred Feature Backlog — explizit vorgemerkt, NICHT in diesem Release):**
  1. **10-Bit HDR End-to-End Streaming** (8-Bit HEVC HW-Encode/-Decode ist seit `v1.5.0.53` aktiv).
  2. **WASAPI Loopback Audio-Streaming** (System-Ton Surface -> PC).
  3. **Seamless Edge KVM** (automatischer Cursor-Übergang an Monitor-Kanten ohne Klick/Shortcut-Toggle).
  4. **In-Stream Quick-HUD Overlay** (weitere Erweiterungen; Basis-HUD bereits aktiv).
  5. **Desktop-Profile** (gespeicherte Presets für Home/Office/Gaming inkl. Auflösungs-, Codec-, KVM- & Power-Kombinationen).
- **Compiler-, INF- & Test-Realität:**
  - *Build & INF:* Windows-x64 Release-Build via `.github/workflows/ci.yml` (Run `45419971886`), `infverif.exe` & `Inf2Cat.exe` OK, Inno Setup 6.7.1 OK (`Screen2PC-Setup-x64.exe` + `Screen2PC-Windows-x64.zip`), GitHub Release **`v1.5.0.57`**, **`Screen2PC.Tests.exe` 21/21 PASS** (`ALL SCREEN2PC TESTS PASSED!`).
- **Next Step:** **`Screen2PC-Setup-x64.exe` (`v1.5.0.57`)** auf Laptop (Rolle *Laptop*) und — sobald verfügbar — auf dem Gaming-PC (Rolle *PC*) testen: Installation nach `C:\Program Files\Screen2PC`, Akku-Autostart nach Neustart sowie Streaming (`v1.5.0.54+` Reconnect, Direct AMF/HEVC, UDP+FEC).

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440` (HDR), Monitor 2: `1920x1080` (SDR).
- **Kernfunktionen:**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im randlosen Vollbild (`WS_POPUP`) oder Fenstermodus (`WS_OVERLAPPEDWINDOW`) inkl. `scRGB`-HDR- & SDR-Farbanpassung dargestellt.
  - Automatischer Windows-x64-Release-Build + Tests + INF/CAT-Prüfung + GitHub Release (`Screen2PC-Windows-x64.zip`) via GitHub Actions; integrierter 1-Klick Auto-Updater auf PC & Surface.
  - Einmalige 6-stellige PIN-Kopplung mit MAC-Merkliste & Vertrauensverwaltung, bidirektionale Zwischenablage (Text/Bilder), rekursiver Ordner- & Datei-Transfer mit Fortschritt/Speed/Abbruch, nahtloses Full-Desktop-KVM (ohne PC-Treiber/Hooks) sowie Smart-Lid-Close-Guard + optionalem Surface-Dim-Overlay.

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (`stdcpp20`, `x64`-only, `/MT` Release / `/MTd` Debug, VS 2022 `v143`)**
   - *Stack:* Win32 + D3D11 + DXGI 1.6 (`IDXGIOutput6`, `ALLOW_TEARING`, Waitable SwapChain) + Media Foundation + Winsock2 + `IPHelper` + `BCrypt` + `PowrProf` + WinHTTP + `winmm` (`timeBeginPeriod(1)`) + UMDF v2 (`2.15`, `IddCx 1.2`), `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`.
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, schont RAM auf dem 8-GB-Surface, verhindert GC-Mikroruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen `Screen2PC.sln`.
2. **PC bleibt komplett treiberfrei (Pure User-Mode KVM, entkoppelter Render-Loop & Task-Manager Autostart):**
   - `Screen2PC.Receiver.exe` installiert keinen Treiber und nutzt kein `SetWindowsHookEx`.
   - **Autostart (`Autostart.h/.cpp`):** Trägt `"Screen2PC Receiver"` unter `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` mit `"<exe>" --tray` ein und hält `StartupApproved\Run` synchron (`WM_ACTIVATE`-Sync).
   - **`D3D11Renderer` Low-Latency & KVM-Capture:** `FLIP_DISCARD` + `ALLOW_TEARING` + `FRAME_LATENCY_WAITABLE_OBJECT`, GPU-NV12/BGRA-Pfade, `MsgWaitForMultipleObjectsEx`-Schleife auf `FRAME_LATENCY_WAITABLE_OBJECT` + neuem Frame-Event + Window-Messages, Streamfenster beim `WM_CLOSE` nur versteckt (`hiddenByUser`, per Button/Tray reaktivierbar). KVM nutzt ungedrosselte HID-Relativdeltas (`WM_INPUT` + `ClipCursor` + `SetCursor(nullptr)`) + `StepVirtualDesktopPoint`.
3. **HDR- & Pro-Monitor-Farbanpassung (`D3D11Renderer`, `ColorProfile.h/.cpp`, `CodecH264.cpp` — 100 % treiberfrei):**
   - `CodecH264` erzwingt BT.709-Colorimetry (`MF_MT_YUV_MATRIX`/`PRIMARIES`/`TRANSFER_FUNCTION`), `D3D11Renderer` wählt je Zielmonitor `R16G16B16A16_FLOAT` (`scRGB` `G10_P709`) mit Gamma `2.2` + `DISPLAYCONFIG_SDR_WHITE_LEVEL` + Live-Shader-Sliders (`receiver.ini`).
4. **LAN-Sicherheit & Protokoll v2 (`Protocol.h`, `Pairing.h/.cpp`, `NetSocket.h/.cpp`):**
   - **Protokoll `v2` (`kProtocolVersion = 2`):** `HelloSender` trägt Sender-Name, App-Version, lokale MAC & persistente `DeviceId` (`sender.ini`); bei Versionskonflikt liefert `RecvPacketRaw` bzw. Discovery explizite `VersionMismatch`-Details inkl. Updater-Prompt.
   - **PIN + MAC-Trust:** Unbekannte Sender erhalten `PairChallenge` + einmalige 6-stellige PIN am PC (`Screen2PCPinPopupWnd`, max. 3 Versuche); nach erfolgreicher Prüfung wird die MAC (bzw. `DeviceId`) in `receiver.ini` `[TrustedDevices]` gesichert (`Screen2PCTrustedWnd`). Eine bestehende Session wird erst **nach** bestandener Authentifizierung abgelöst.
5. **Low-Latency H.264, Live-Reconfig & Smart Power Policy (`CodecH264`, `SenderEngine`, `PowerPolicy`, `BitratePresets.h`):**
   - `LowDelayVBR` + 3-Slot-NV12-Ring + dynamische ABR-Steuerung (`SetBitrate`) + explizite FPS-Presets (`Auto / 60 / 90 / 120 FPS`). Änderungen an Codec, Bitrate oder FPS greifen per `ReconfigureStream` / `StreamConfigChange` sofort ohne Neuverbinden.
   - **Surface Power Guard (`LidCloseGuard` & `SurfaceDimOverlay`):** Sichert Original-AC/DC-LidClose-Werte vor Änderung atomar in `sender.ini` (`[PowerRestore]`), aktiviert `0` (*Do Nothing*) + `ES_SYSTEM_REQUIRED` **nur** solange `st.connected && st.usingExternalDisplays`, restauriert bei Trennung/Beenden/Crash.
6. **GitHub Releases & Auto-Updater (`Version.h`, `Directory.Build.props`, `Updater.h/.cpp`, `ci.yml`):**
   - `Version.h` (`1.5.0.<SCREEN2PC_BUILD_NUMBER>`), WinHTTP-Abfrage gegen GitHub Releases + atomares PowerShell-Self-Update inkl. SHA256-Prüfung des VDD-Treibers am Sender.
7. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx 1.2`):**
   - Deterministische `MonitorContainerId` (`MakeDeterministicMonitorGuid(i)`) & `8 ms`-Event-Wait, Named Pipe `\\.\pipe\Screen2PC_VDD`, Persistenz in `C:\ProgramData\Screen2PC\monitors.cfg`.

---

## 3. Repository Map
| Pfad | Inhalt & Zweck |
|---|---|
| `MEMORY.md`, `README.md` | Zentrale Projekt-Memory (`MEMORY.md`) & Enduser-Schnellstart (`README.md`) |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props`, `.gitignore` | VS 2022 Solution (`x64`, `SCREEN2PC_BUILD_NUMBER`), NuGet-WDK/-SDK `10.0.26100.2454` für Driver |
| `src/common/` (`Screen2PC.Common.vcxproj`) | `Protocol.h` (`v3`), `Version.h`, `Resource.h`, `AppIcon.h`, `BitratePresets.h`, `assets/Screen2PC.ico`, `AmfMinimal.h`, `AmfEncoder.*` (Direct `amfrt64.dll` H.264/HEVC), `UdpVideoFec.*` (GF(2⁸) Cauchy RS-FEC), `Autostart.*`, `ColorProfile.*`, `Pairing.*`, `Updater.*`, `Logger.*`, `NetSocket.*`, `CodecH264.*` (AMF/MF H.264+HEVC), `CodecDelta.*` (`DLT1`/`DLT2` XOR-LZ4 + `DLT3` QOI hybrid), `ClipboardSync.*`, `FileTransfer.*`, `KvmInput.*`, `UiTheme.*`, `Lang.h` |
| `src/driver/` (`Screen2PC.Driver.vcxproj`) | `DriverProtocol.h` (`MakeDeterministicMonitorGuid`), `Driver.h/.cpp` (UMDF v2 `IddCx 1.2`), `Screen2PCDriver.inf` |
| `src/driversetup/` (`Screen2PC.DriverSetup.vcxproj`) | `main.cpp` -> `Screen2PC.DriverSetup.exe` |
| `src/sender/` (`Screen2PC.Sender.vcxproj`) | Surface-App: `SenderEngine.*` (PIN-Flow, Live-Reconfig, ABR, UDP+FEC Sender), `DxgiCapture.*`, `GpuDeltaAnalyzer.*` (`cs_5_0` 64×64 Tile Diff), `PowerPolicy.*`, `VirtualDisplayCtl.*`, `Screen2PC.Sender.rc`, `main.cpp` (`CodecIdx 0=H264 / 1=HEVC / 2=Delta`, `15..200 Mbit/s`, `Auto/60/90/120 FPS`) |
| `src/receiver/` (`Screen2PC.Receiver.vcxproj`) | PC-App (treiberfrei): `ReceiverEngine.*` (`WSAPoll` TCP+UDP Video+FEC Reassembly, H.264/HEVC/Delta Decode, PIN/MAC-Auth), `D3D11Renderer.*` (Direct NV12 Shader, `ALLOW_TEARING`, HDR/`scRGB`, Quick-HUD), `Screen2PC.Receiver.rc`, `main.cpp` (Firewall inkl. UDP `48216–48217`) |
| `src/tests/` (`Screen2PC.Tests.vcxproj`) | `main.cpp` -> `Screen2PC.Tests.exe`: 21 Suiten (inkl. `TestDeltaQoiHybrid`, `TestUdpVideoFec`, `TestAmfFallbackAndHevcHeaders`) |
| `scripts/` | `install_driver.ps1`, `uninstall_driver.ps1`, `generate_icons.py` |
| `installer/` | `Screen2PC.iss` — Inno Setup 6 Windows-x64-Installer (`Screen2PC-Setup-x64.exe`: Laptop/PC-Rollen, Treiber/Firewall, `--install-autostart`, Uninstaller) |
| `.github/workflows/ci.yml` | Windows-2022 CI: MSBuild `Release\|x64`, Tests, `infverif`, `Inf2Cat`, Inno Setup 6, Artifact & GitHub Release (`Screen2PC-Setup-x64.exe` + `Screen2PC-Windows-x64.zip`) |

---

## 4. Konfiguration, Hotkeys, Firewall & Pfade
- **Sender Display-Modi (`DisplayMode`):** `ExtendAll` (`0`), `ExtendPrimary` (`1`), `MirrorPrimary` (`2`), `ExtendSecondary` (`3`).
- **Hotkeys & Bedienung:**
  - *Sender (`540x740` @ 96 DPI, Per-Monitor DPI v2):* Globaler Hotkey `Ctrl+Shift+K`; `X` minimiert ins Tray; `Codec / Bitrate / FPS` live schaltbar; Checkboxen für Zwischenablage, KVM, Task-Manager-Autostart, `AutoConnect`, `AutoReconnect`, `LidCloseBypass`, `DimSurfaceOnExternal`, Auto-Update; Datei-/Ordner-Transfer + Progress/%/`MB/s` + *"Abbrechen"*.
  - *Receiver (`580x684` @ 96 DPI, Per-Monitor DPI v2):* `X` versteckt ins Tray; Slider für **Maus-Tempo (KVM)** (`25..500 %`, Standard `150 %`) & **Scroll-Tempo (KVM)** (`25..500 %`, Standard `200 %`) direkt am PC (Surface-Touchpad bleibt unberührt); Buttons für Session-Trennung, *"Streamfenster wieder einblenden"*, *"Farbe & HDR..."*, *"Vertraute Geräte..."*, Firewall & Updates; nicht-blockierendes PIN-Popup (`Screen2PCPinPopupWnd`); Datei-/Ordner-Transfer + Progress/%/`MB/s` + *"Abbrechen"*.
  - *Streamfenster (`D3D11Renderer`):* `F11` Vollbild-Toggle; `ESC` verlässt Vollbild **und** gibt KVM-Mausfokus frei; Klick ins Bild aktiviert Full-Desktop-KVM-Capture (`WM_INPUT` + `ClipCursor`); `Ctrl+Alt+K` schaltet KVM global um; `X` / `Alt+F4` versteckt das Streamfenster nur (Session läuft weiter, reaktivierbar über *"Streamfenster wieder einblenden"*).
- **INI-Konfiguration, Registry & Pfade:**
  - `%APPDATA%\Screen2PC\sender.ini` (`[Sender]`): `LastIp=""`, `ModeIdx=0`, `CodecIdx=0` (`0=H264, 1=HEVC, 2=DeltaLossless`), `BitrateMbps=80` (`BitrateIdx=5`), `FpsSetting=0` (`FpsIdx=0`), `ClipboardSync=1`, `KvmEnabled=1`, `AutoConnect=0`, `AutoReconnect=1`, `LidCloseBypass=1`, `DimSurfaceOnExternal=0`, `AutoUpdateCheck=1`, `DeviceId=""`; `[PowerRestore]`: `Pending`, `SchemeGuid`, `SavedAc`, `SavedDc`.
  - `%APPDATA%\Screen2PC\receiver.ini` (`[Receiver]`): `StartFullscreen=1`, `ClipboardSync=1`, `KvmEnabled=1`, `AutoUpdateCheck=1`, `KvmMouseSpeedPct=150`, `KvmScrollSpeedPct=200`; `[ColorMonitor0]`, `[ColorMonitor1]`: `HdrOutput=1`, `BrightnessPct=100`, `GammaX100=220`, `ContrastPct=100`, `SaturationPct=100`, `RedGainPct=100`, `GreenGainPct=100`, `BlueGainPct=100`; `[TrustedDevices]`: `MAC_<12hex>` / `ID_<32hex> = Name|MAC|DeviceId|LastIp|AddedUtc`.
  - Registry (je optional per Checkbox): `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Screen2PC Receiver` / `Screen2PC Sender` & `StartupApproved\Run`.
  - `C:\ProgramData\Screen2PC\monitors.cfg`, `%USERPROFILE%\Downloads\Screen2PC`, `%TEMP%\Screen2PC_*.log`, `%TEMP%\Screen2PC_Update_<pid>\`.

---

## 5. Bekannte technische Eigenheiten & Grenzen
1. **Receiver-Session-Limits & Auth-Reihenfolge:** Receiver akzeptiert nur **1** aktive Session gleichzeitig; eine vorherige Session wird bei Neuverbindung erst getrennt, **nachdem** Protokollversion und PIN/MAC-Vertrauen erfolgreich geprüft wurden.
2. **Input/UIPI & Hotkey-Details:** `SendInput` am Surface unterliegt Windows-UIPI (ohne Admin-Start keine Steuerung von UAC-/Admin-Fenstern); beim Verlassen via `ESC` wird `m_suppressEscUp` lokal abgefangen. `F11` prüft Repeat-Bit und `Ctrl+Shift+K` (`RegisterHotKey`) nutzt `MOD_NOREPEAT`; beim Trennen/Fokusverlust setzt `ReleaseRemoteModifiers` hängende Remote-Modifier (`Ctrl`/`Alt`/`Shift`/`Win`) sauber zurück.
3. **Codec-/Clipboard-/Socket-Grenzen & Recovery:** Runtime-Video-Fallback auf Sender (`AMF -> MF HW -> MF SW / H.264 -> Delta`) und Receiver (`DXVA -> CPU`) greift nur vor dem ersten erfolgreich dekodierten Frame; dynamische Auflösungswechsel und GPU/DXGI-Device-Lost (`DXGI_ERROR_DEVICE_REMOVED`/`RESET`) erneuern Capture, Codec und `D3D11Renderer` automatisch im Betrieb. `ClipboardSync` kappt Text bei `2 MB` (`NormalizeTextPayload`, `CF_DIB` bis `16 MB`); `FileTransfer` kappt einzelne Dateien bei `4 GB` und blockiert Path-Traversal (`..`, Laufwerks-/UNC-Pfade).
4. **Protokoll-Kompatibilität v3:** Da Protokoll `v3` (`>= v1.5.0.53`) HEVC (`CodecType::HEVC`) und UDP+FEC-Video (`48216–48217`) voraussetzt, zeigen ältere Gegenstellen gezielt die Meldung **"Versionskonflikt"** inkl. GitHub Update-Angebot. Auf beiden Rechnern auf den gleichen Stand aktualisieren.

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
| 2026-11-16 | Surface Sender Autostart (Variante A) | Gleicher Task-Manager-Autostart (`Screen2PC Sender`, `--tray`, `Local\Screen2PC_Sender_SingleInstance`, `WM_ACTIVATE`-Sync) auch in `src/sender/main.cpp` ergänzt (`b0adff2`, Run `43844648845`, Artifact `14215250697`, Release `v1.5.0.26`, 10/10 Tests PASS) |
| 2026-11-16 | Umfassendes Stability-, Deadlock- & Bounds-Debugging | Voll-Audit über `Driver` (Overlapped-Pipe + Monitor-Rollback), `Common` (`ClipboardSync`, `CodecDelta`, `CodecH264` Async-MFT/Multi-MFT, `ColorProfile`, `KvmInput`, `NetSocket` Cancel-Connect/Atomic-UDP, `Protocol` Cursor-Check, `Updater`), `Sender` (`DxgiCapture` Reset/Resize, `SenderEngine` Device-Reset & Interrupt-Handshake, `MOD_NOREPEAT`), `Receiver` (`D3D11Renderer` Device-Lost-Recovery/Modifiers, `ReceiverEngine` Interrupt-Accept, `g_disconnBusy`) & erweiterte Regressionstests (`b0a55ed`, Run `43858802633`, Artifact `14222816676`, Release `v1.5.0.27`, 10/10 Tests PASS) |
| 2026-11-17 | App- & Tray-Icon (Dual-Glass Projection) | Transparentes Multi-Size-Monitor-Icon (`16/20/24/32/48/64`-px ARGB-BMP + `256×256`-PNG via `scripts/generate_icons.py` -> `src/common/assets/Screen2PC.ico`), `Resource.h` (`IDI_SCREEN2PC_APP`), `AppIcon.h` (`LoadAppLargeIcon`/`LoadAppSmallIcon`), Einbindung in `Sender`/`Receiver` `.rc`, Hauptfenster, `ColorWnd`, `D3D11Renderer` & System-Trays + `TestAppIcons`-Suite (`a45c016`, Run `44076960795`, Artifact `14324348800`, Release `v1.5.0.28`, 11/11 Tests PASS) |
| 2026-11-17 | Erweiterte Bandbreiten-Abstufungen | `BitratePresets.h` (`15/25/40/50/60/80/100/120/150/200 Mbit/s`), ComboBox + `WS_VSCROLL` & `BitrateMbps`-INI-Speicherung mit Legacy-`BitrateIdx`-Migration in `sender/main.cpp` + `TestBitratePresets`-Suite (`8b5a08d`, Run `44099639218`, Artifact `14335706815`, Release `v1.5.0.29`, 12/12 Tests PASS) |
| 2026-11-17 | Low-Latency H.264, UX-Polish, PIN/MAC-Pairing, Version-Conflict, Folder-Transfer & Smart Lid-Close Guard | H.264 `LowDelayVBR` + 3-Slot-NV12-Ring + ABR + asymmetrische TCP-Puffer + `ALLOW_TEARING`/`Waitable` + entkoppelter Decode-Thread + `MsgWaitForMultipleObjectsEx`; Hide-on-Close + Show-Streams; `MakeDeterministicMonitorGuid`; `kFpsPresets` (`Auto/60/90/120`) + Live-Reconfig; `TaskbarCreated` & DPI-v2; `AutoConnect`/`AutoReconnect`; `Pairing.*` (6-stellige PIN + MAC-Trust-List); `Protocol v2` Versionskonflikt-UI; rekursiver Ordner-Transfer + Speed/%/Cancel; `LidCloseGuard` (Crash-Recovery) + `SurfaceDimOverlay`; 18/18 Tests PASS & Backlog für 5 Folge-Features dokumentiert (`8a57413`, Run `44205372228`, Artifact `14383946286`, Release `v1.5.0.31`) |
| 2026-11-18 | H.264 Multi-Monitor Flicker & Latency Fixes | `SenderEngine` Encoding-Gating auf `!sendQueueOccupied` & natives `nextEncodeQpc`-Pacing; `CodecH264` Async-MFT-Warmup/Keyframe-Fix & `MF_E_NOTACCEPTING`-Drain; `D3D11Renderer` 3-Buffer `FLIP_DISCARD` (`3543036`, Run `44424706329`, Release `v1.5.0.34`) |
| 2026-11-19 | Unabhängiges KVM Maus- & Scroll-Tempo (PC Receiver) | `KvmInput::ScaleRelativeDelta`/`ScaleScrollDelta` (`25..500 %`, Sub-Pixel-Akkumulatoren) + Live-Regler im Receiver (`Maus-Tempo (KVM)` Standard `150 %`, `Scroll-Tempo (KVM)` Standard `200 %`, `receiver.ini`) ohne Surface-Touchpad-Änderung + `TestKvmInput`-Tests (`308cfac`, Run `44428943006`, Release `v1.5.0.35`) |
| 2026-11-19 | Win11 UI-Modernisierung, In-Stream Quick-HUD, Live-Moduswechsel & Ordner-Batch-Fortschritt | `UiTheme.h/.cpp` (ComCtl v6, Dark/Light-Mode, Cards, Toggle-Switches, Tabs, Tooltips), `D3D11Renderer` In-Stream Quick-HUD (`FPS/ms/Mbit/s` + Hover-Buttons + `StreamHudEnabled` Toggle), `SwitchDisplayModeWhileConnected`, `FileTransfer` Gesamt-Ordner-Fortschritt (%) & Abschlussmeldung, `SetupVirtualDisplays` Fallback-Fix (`abeaa91`..`775e566`, Run `44560720571`) |
| 2026-11-19 | `SetupVirtualDisplays` Overlap- & Extend-Retry-Fix | `hasOverlappingPrimary` in `SenderEngine` prüft nun `after` statt veraltetes `before` (verhindert Endlos-Overlap-Erkennung wenn virtueller Monitor `(0,0)` wird), `SetDisplayConfig(SDC_TOPOLOGY_EXTEND)`-Retry (bis zu 3×), 10s-Timeout & `VDD-SETUP`/`SND-VDD`-Logging für die UI-Logbox (`87912c6`, Run `44582348526`, Release `v1.5.0.44`) |
| 2026-11-19 | Public GitHub Prep (Laptop Naming, Bilingual UI & Docs, MIT License) | `Surface` -> `Laptop` in UI/Tray/Scripts/.rc; `Lang.h` (`Auto/DE/EN` live in Sender & Receiver); `LICENSE` (MIT), `.github/ISSUE_TEMPLATE/`, `docs/images/*.png` (6 Screenshots), `README.md` (EN) & `README.de.md` (DE) |
| 2026-11-19 | UI-Flicker-Fix (250-ms-Telemetrie-Timer) | `SetDlgItemTextIfChanged`, `EnableDlgItemIfChanged` & `SetProgressIfChanged` in `UiTheme.h` verhindern unnötiges Neuzeichnen unveränderter Labels/Buttons/Progressbars; `WS_EX_COMPOSITED` auf beiden Hauptfenstern (`sender/main.cpp`, `receiver/main.cpp`) sorgt für flackerfreies Double-Buffering auch bei Live-Updates (`8af6fe7`, Run `44630654398`, Release `v1.5.0.47`) |
| 2026-11-19 | H.264-Grünstich-Flacker-Fix | Cross-Codec-`DeltaLossless`-Settle (`h264NeedsSettle` nach 150 ms) im H.264-Modus in `SenderEngine::RunStreamWorker` entfernt; H.264 bleibt konsistent im NV12-Pfad ohne 1-2-LSB-YUV/RGB-Grünstich-Sprünge bei jedem neuen Frame (`e30ba0d`, Run `44634411677`, Release `v1.5.0.49`) |
| 2026-11-23 | Pipeline-Latenz- & Delta-Unschärfe-Fix | `SenderEngine`/`ReceiverEngine`: TCP-Puffer auf `64 KB` / `128 KB` gestrafft, Dual-Monitor-Bitraten-Split (`60%/40%`) & `60 FPS` Auto-Cap, adaptives Encode/Send-Pacing + schnelleres ABR (`10 ms`, Floor `8 Mbit/s`), non-blocking Cursor-Shape-Queue, Burst-Drain-Coalescing & partielles `QueueCpuFrame`; `CodecH264`: 1.5-Frame-VBV & `Flush()` nach `VideoProcessorBlt`; `CodecDelta`/`DxgiCapture`: `O(tiles)`-Grid-Run-Coalescing, 16-KB-L1-LZ4-Hash, `CopyFrameToCpuRegions`, Sustained-Only-2×2 & nicht-abbrechendes Settle (`3cc0af9`, Run `45200582324`, Release `v1.5.0.50`) |
| 2026-11-23 | Direct AMF (`amfrt64.dll`), HEVC (H.265), GPU Compute Delta, `DLT3` QOI & UDP+RS-FEC | `AmfMinimal.h` / `AmfEncoder.*` direkt auf `amfrt64.dll`; `CodecH264` + UI + `ReceiverEngine` für `CodecType::HEVC`; `GpuDeltaAnalyzer.*` (`cs_5_0` 64×64 Tile Diff + Bounding-Box-Readback); `CodecDelta` `DLT3` QOI-Lossless-Hybrid; `UdpVideoFec.*` + `Protocol v3` UDP `48216–48217` RS-FEC mit TCP-Fallback & Firewall-Update (`7c2d6db`, Run `45221116035`, Release `v1.5.0.53`, 21/21 Tests PASS) |
| 2026-11-24 | Stream-Timeout- & Auto-Reconnect-Fix | TCP-Stream-Timeouts (`48212–48213`) auf `6000 ms` und Post-Handshake-Control-Timeouts (`48211`) auf `4000 ms` angehoben; `NetSocket::IsPeerClosed()`, automatischer Stream-Socket-Reconnect in `SenderEngine::RunStreamWorker` & `ReceiverEngine::RunStreamWorker` bei aktivem Control-Channel (Streamfenster/`D3D11Renderer` bleibt offen, Decoder/FEC-Reset + `RequestKeyframe`, `expectedPeerIp`-Prüfung, `inBlockingSend`-Shutdown-Guard) + `TestLoopbackNetworking`-Reconnect-Tests (`76a42bc`, Run `45411535572`, Release `v1.5.0.54`, 21/21 Tests PASS) |
| 2026-11-24 | Akku-/UIPI-fester Sender-Autostart & Tray-Resilienz | `Autostart.h/.cpp`: `BuildSenderTaskXml` registriert `"Screen2PC Sender"` per UTF-16LE Task-XML mit `DisallowStartIfOnBatteries=false`, `StopIfGoingOnBatteries=false`, `StartWhenAvailable=true`, `ExecutionTimeLimit=PT0S`, User-SID & `HighestAvailable`; `RefreshAutostartPathIfNeeded` migriert alte `HKCU\Run`-Einträge und aktualisiert bestehende Tasks automatisch (`NeedsSenderTaskUpgrade`); `RunSchtasks` unterstützt UTF-16LE/UTF-8/OEM; `sender/main.cpp` & `receiver/main.cpp`: `ChangeWindowMessageFilterEx` (`TaskbarCreated` / `g_wmShowExisting`) + `TIMER_TRAY_RETRY` (60×1 s) + erweiterte `TestAutostartAndCli`-Tests (`daf8f3b`, Run `45414540566`, Release `v1.5.0.55`, 21/21 Tests PASS) |
| 2026-11-24 | Inno Setup Windows-x64-Installer (`Screen2PC-Setup-x64.exe`) | `installer/Screen2PC.iss` & `.github/workflows/ci.yml`: zweisprachiger Wizard (DE/EN), exklusive Rollen *Laptop* (`Sender` + `install_driver.ps1` + Task-XML-Autostart) vs. *PC* (`Receiver` 100 % treiberfrei + Firewall TCP `48211–48215` / UDP `48210,48215–48217` + `runasoriginaluser` Autostart), Installation nach `C:\Program Files\Screen2PC`, `[InstallDelete]` bei Rollenwechsel & sauberer Deinstaller; `--install-autostart`/`--uninstall-autostart` & `WM_QUERYENDSESSION`/`WM_ENDSESSION` in `sender/main.cpp` & `receiver/main.cpp`; Release liefert `Screen2PC-Setup-x64.exe` (`2.62 MB`) + `Screen2PC-Windows-x64.zip` (`1.31 MB`) für den In-App-Updater (`9179b94`, Run `45419971886`, Release `v1.5.0.57`, 21/21 Tests PASS) |
