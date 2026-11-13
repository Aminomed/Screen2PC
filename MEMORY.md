# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `Phase 0–3 Komplett, CI-verifiziert & Full-Codebase-Audit synchronisiert`
- **Status:** Alle Kern- & Ausbauphasen (UMDF2 Virtual Display Driver `IddCx 1.2`, H.264/DXVA + BGRA-Delta-Lossless-Streaming, Shared Clipboard, Full-Duplex Quick File Transfer, Treiberfreies User-Mode-KVM inkl. `WaitReadable(10)`-Loop) sowie alle Deep-Audits & Hardening-Fixes (Code-HEAD `1a84fae`, vorheriger Doku-Stand `f442f05`) sind implementiert und in GitHub Actions Run `43375193571` (`2026-11-13 05:55 UTC`, `Release|x64`, Artifact `Screen2PC-Windows-x64.zip` ID `14014620658`, `812.139` Bytes; Build-Run auf `1a84fae` war `43374918851`) grün verifiziert.
- **Compiler-, INF- & Test-Realität:**
  - *Build & INF:* **0 Errors** und **0 Warnings** in `Common`, `DriverSetup`, `Sender`, `Receiver` und `Tests`. In `Screen2PC.Driver` erscheinen 2 erwartbare WDK-/INF-Meldungen (`WudfWdm.h` `C4471` aus WDK `2.15` und `Screen2PCDriver.inf(30)` `warning 2084` zu `%12%\WUDFRd.sys`); `infverif.exe /v` (`INF is VALID`) und `Inf2Cat.exe /os:10_VB_X64 /uselocaltime` laufen grün durch.
  - *Test-Scope:* `Screen2PC.Tests.exe` besteht **7/7 Testsuiten** (`81 CHECK`-Assertions) gegen `Screen2PC.Common` (inkl. CPU-MFT-H.264, Loopback-TCP/UDP, Full-Duplex-FileTransfer & KVM-Mathematik). UI, `DxgiCapture`, `D3D11Renderer`, UMDF2-Treiber, reales Hardware-DXVA, echtes `SendInput` sowie `HWND_MESSAGE`-Clipboard sind durch CI nicht abgedeckt.
- **Next Step:** Echter Ende-zu-Ende-Hardwaretest auf **Surface (Sender)** + **Gaming-PC (Receiver)** mit dem Release-Bundle `Screen2PC-Windows-x64.zip` (bei Bedarf optionale Fixes für die in Sektion 5 dokumentierten Audit-Eigenheiten).

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440`, Monitor 2: `1920x1080`.
- **Kernfunktionen (Nachtschicht-Ziel bis 09:00 CET am 2026-11-13 erreicht; Commits 2026-11-12 23:20 – 2026-11-13 05:55 UTC):**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im randlosen Vollbild (`WS_POPUP`) oder Fenstermodus (`WS_OVERLAPPEDWINDOW`) dargestellt.
  - Automatischer Windows-x64-Release-Build + Tests + INF/CAT-Prüfung via GitHub Actions bei jedem Push/PR (inkl. `Screen2PC-Windows-x64.zip`).
  - Bidirektionale Zwischenablage (Text/Bilder), Full-Duplex Quick File Transfer (PC <-> Surface) und KVM-Eingabe (PC-Maus/Tastatur -> Surface, komplett treiberfrei auf dem PC).

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (`stdcpp20`, `x64`-only, `/MT` Release / `/MTd` Debug, VS 2022 `v143`)**
   - *Stack:* Win32 + D3D11 + DXGI 1.2 + Media Foundation + Winsock2 + UMDF v2 (`2.15`, `IddCx 1.2`), `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`.
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, schont RAM auf dem 8-GB-Surface, verhindert GC-Mikroruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen `Screen2PC.sln`.
2. **PC bleibt komplett treiberfrei (Pure User-Mode):**
   - `Screen2PC.Receiver.exe` installiert keinen Treiber und nutzt kein `SetWindowsHookEx`. KVM erfasst Maus/Tastatur ausschließlich über reguläre `WM_*`-Fenstermeldungen der aktiven `D3D11Renderer`-Fenster (vollkommen Anti-Cheat-kompatibel).
3. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx 1.2`):**
   - Minimaler User-Mode-Display-Treiber ohne eigenes Kernel-`.sys` (`Screen2PCDriver.inf` bindet Microsoft-Inbox `%12%\WUDFRd.sys` + `UpperFilters = "IndirectKmd"`, `HardwareID = Root\Screen2PCVDD`, `PnpLockdown = 1`, `MaxMonitorsSupported = 2`).
   - *IPC:* Named Pipe `\\.\pipe\Screen2PC_VDD` (SDDL `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)`, `nMaxInstances = 4` mit sequenziellem Connect-Loop, `SanitizeSpec` klemmt auf `640..7680 x 480..4320 @ 30..240 Hz` bzw. `1920x1080@60`).
   - *Persistenz (Boot/D0-Recovery):* `VirtualDisplayCtl::SetMonitors()` schreibt den `VddCommand` vor dem Pipe-Call hart nach `C:\ProgramData\Screen2PC\monitors.cfg` (bei `ClearMonitors()` / `uninstall_driver.ps1` gelöscht). Der Treiber liest diese Datei einmalig in `OnAdapterInitFinished()` — sie ist **kein** Laufzeit-Fallback bei fehlgeschlagenem Pipe-Call.
   - *Anzeige-Aktivierung & Code Integrity:* `SenderEngine::SetupVirtualDisplays()` pollt DXGI bis `5500 ms` (200-ms-Intervall) und erzwingt nach `1600 ms` einmalig `SetDisplayConfig(..., SDC_APPLY | SDC_TOPOLOGY_EXTEND)`. Receiver sortiert lokale Monitore `Primary -> Links -> Oben` (`<24 Hz` -> `60 Hz`, Cap `kMaxMonitors = 2`). Blockiert Windows den selbstsignierten Treiber auf dem Surface (Code 52), ist nur am Surface `bcdedit /set testsigning on` nötig (alternativ treiberfreier `MirrorPrimary`-Modus).
4. **Dual-Codec & Rendering-Pipeline:**
   - **Standard (`CodecType::H264`; `Auto` mappt auf `H264`):** Low-Latency Media Foundation (`MF_LOW_LATENCY`, `CODECAPI_AVLowLatencyMode`, GOP = `fps` / 60, 0 B-Frames, Mindestbitrate `2 Mbit/s`, gerade Dimensionen `w & ~1`, `h & ~1`).
     - *Sender:* Dropdown-Presets **`15 / 25 (Default) / 40 / 60 Mbit/s`** bei fest `60 FPS`. `H264Encoder::Init` probiert GPU-MFT (`MFT_ENUM_FLAG_HARDWARE` + D3D11 `VideoProcessorBlt` BGRA->NV12 via `IMFDXGIDeviceManager`), sonst Software-MFT (`SYNCMFT` / `CLSID_CMSH264EncoderMFT`). Bei Init-Fehler -> `DeltaLossless`. Im `StreamWorker` erfolgt vor dem ersten gesendeten Frame (`!sentInitialFrame`) nach 4 `EncodeTexture`-Fehlern der Runtime-Fallback `GPU H.264 -> CPU H.264 -> DeltaLossless`.
     - *Receiver:* `H264Decoder::Init` nutzt `MFT_ENUM_FLAG_SYNCMFT` (Fallback `CLSID_CMSH264DecoderMFT`) und bindet bei `MF_SA_D3D11_AWARE` DXVA/D3D11 + `VideoProcessorBlt` mit `MF_MT_MINIMUM_DISPLAY_APERTURE`-Crop (trennt z. B. `1088 -> 1080`, `368 -> 360`). Im `ReceiverEngine` nach 4 initialen DXVA-Fehlern (`!decodedAnyH264`) Runtime-Fallback auf CPU (`allowHardware = false` + `CpuNv12ToBgra`) plus 1s-gedrosselte `RequestKeyframe`-Recovery ab >=2 Fehlern.
     - *D3D11Renderer:* Hardware-D3D11 mit WARP-Fallback, `ID3D10Multithread`, Flip-Model `DXGI_SWAP_EFFECT_FLIP_DISCARD` (2 Backbuffer), `DXGI_MWA_NO_ALT_ENTER`, `Present(0, 0)` (ohne VSync), HLSL `vs_5_0`/`ps_5_0` Letterbox/Pillarbox-Quad + alpha-geblendeter DXGI-Hardware-Cursor (`Mono` Type `1`, `Color` Type `2`, `MaskedColor` Type `4`).
   - **Sharp-Modus (`CodecType::DeltaLossless`):** Verlustfreier **32-Bit-BGRA**-Codec (`DLT1` Magic, 128er Run-/Literal-RLE-Caps; nutzt DXGI `MoveRects` + `DirtyRects` als Hints, sonst 64x64-Tile-Diff).
5. **Transport & Wire-Protokoll (`Protocol.h`, `NetSocket`):**
   - IPv4-only (`AF_INET`), Packet-Magic `S2P1` (`0x53325031`), Version `1`, `kMaxFrameBytes = 64 MB`, **ohne TLS/Auth** (nur vertrauenswürdiges lokales LAN).
   - **Ports:** UDP `48210` (Discovery Broadcast an alle Interface-Broadcasts + `255.255.255.255`), TCP `48211` (Control, Handshake, 2s-Sender-Heartbeat / 8s-Receiver-Timeout, `ClipboardSync`, `KvmMouse`/`KvmKey`, `RequestKeyframe`, `StopSession`), TCP `48212` / `48213` (`kPortStreamBase + i` pro Monitor, `TCP_NODELAY`, 4-MB-Buffer), TCP `48214` (`FileTransfer`, Full-Duplex, 64-KB-Chunks + Chunk- & Gesamt-CRC32, max `4 GB`, 2-MB-Buffer).
   - `TcpSocket::m_sock` als `std::atomic<SOCKET>` für sicheres `Close()`; `SendPacket` bündelt Header+Payload bis `1536 B` auf dem Stack; kein Auto-Reconnect.

---

## 3. Repository Map
| Pfad | Inhalt & Zweck |
|---|---|
| `MEMORY.md`, `README.md` | Zentrale Projekt-Memory (`MEMORY.md`) & Enduser-Schnellstart (`README.md`) |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props`, `.gitignore` | VS 2022 Solution (6 Projekte, `Debug\|x64` & `Release\|x64`), NuGet-WDK/-SDK `10.0.26100.2454` für `Screen2PC.Driver`, Ignore-Regeln (`out/`, `dist/`, `packages/`) |
| `src/common/` (`Screen2PC.Common.vcxproj`) | `Protocol.h`, `Logger.h/.cpp` (`300`-Zeilen-Ringpuffer), `NetSocket.h/.cpp`, `CodecH264.h/.cpp`, `CodecDelta.h/.cpp`, `ClipboardSync.h/.cpp` (`HWND_MESSAGE`, 64-Bit-FNV-1a-Anti-Echo, 3x 15ms-Retry), `FileTransfer.h/.cpp` (`.part_`-Staging, Path-/Drive-Sanitization, Auto-Rename `1..9999`), `KvmInput.h/.cpp` (`SendInput` Virtual-Desktop) |
| `src/driver/` (`Screen2PC.Driver.vcxproj`) | `DriverProtocol.h`, `Driver.h/.cpp` (UMDF v2 `IddCx 1.2`, Pipe-Server, `IndirectMonitorContext`), `Screen2PCDriver.inf` (`SignMode=Off`, Post-Link-DLL-Kopie für `Inf2Cat`) |
| `src/driversetup/` (`Screen2PC.DriverSetup.vcxproj`) | `main.cpp` -> `Screen2PC.DriverSetup.exe`: `--install <inf>`, `--uninstall` (nur PnP-Devnodes), `--status`, `--ping`, `--set <0\|1\|2>` (Test-Specs `2560x1440@60` / `1920x1080@60`) |
| `src/sender/` (`Screen2PC.Sender.vcxproj`) | Surface-App: `SenderEngine.h/.cpp`, `DxgiCapture.h/.cpp`, `VirtualDisplayCtl.h/.cpp`, `main.cpp` (Win32 GUI/Tray, `Ctrl+Shift+K`, Drag&Drop) |
| `src/receiver/` (`Screen2PC.Receiver.vcxproj`) | PC-App: `ReceiverEngine.h/.cpp`, `D3D11Renderer.h/.cpp`, `main.cpp` (Win32 GUI/Tray, Multi-Monitor-Enum max `2`, `F11`/`ESC`/`Ctrl+Alt+K`, Firewall-UAC-Button, Drag&Drop) |
| `src/tests/` (`Screen2PC.Tests.vcxproj`) | `main.cpp` -> `Screen2PC.Tests.exe`: 7 Suiten (`TestProtocol`, `TestDeltaCodec`, `TestColorAndH264`, `TestLoopbackNetworking` `48299`/`48298`, `TestClipboardSync` rein statisch, `TestFileTransfer` Full-Duplex+EOF `48297`, `TestKvmInput` rein mathematisch) |
| `scripts/` | `install_driver.ps1` (Admin, lokales Cert `CN=Screen2PC Local Virtual Display` in `My`+`Root`+`TrustedPublisher`, SHA256 Authenticode `.dll`/`.cat`, `icacls C:\ProgramData\Screen2PC` `*S-1-5-11:(OI)(CI)M`, `--install`/`--status`/`--ping`) & `uninstall_driver.ps1` (`--uninstall` + löscht `monitors.cfg`) |
| `.github/workflows/ci.yml` | Windows-2022 CI: NuGet-Restore, WDK-`stampinf.exe` in `PATH`, MSBuild `Release\|x64`, `Screen2PC.Tests.exe`, `infverif.exe /v`, `Inf2Cat.exe /os:10_VB_X64 /uselocaltime`, Artifact `Screen2PC-Windows-x64.zip` (inkl. `Tests.exe` & `README.md`) |

---

## 4. Konfiguration, Hotkeys, Firewall & Pfade
- **Sender Display-Modi (`DisplayMode`):** `ExtendAll` (`0`, bis zu 2 virtuelle Monitore), `ExtendPrimary` (`1`, nur PC-Monitor 1), `MirrorPrimary` (`2`, spiegelt Surface-Hauptdisplay ohne Treiber), `ExtendSecondary` (`3`, nur PC-Monitor 2 via Stream-Port `48212` mit `monitorIndex = 1` -> PC-Monitor 1 bleibt frei).
- **Hotkeys & Bedienung:**
  - *Sender (`530x564`):* Globaler Hotkey `Ctrl+Shift+K` (Verbinden/Trennen inkl. Re-Entry-Guard); `X` minimiert ins Tray (`Fenster oeffnen`, `Verbinden`/`Trennen`, `Beenden`); Auto-Discovery beim Start (`800 ms`) & per *"Suchen"* überschreibt `IDC_IP_EDIT`. Drag&Drop (`WM_DROPFILES`) & *"Datei(en) an PC senden..."* (`GetOpenFileNameW`, 32k-Puffer).
  - *Receiver (`570x474`, Multi-Instance blockiert durch Listen `48211`):* `X` versteckt ins Tray (`Statusfenster oeffnen`, `Beenden`); Button *"Windows Firewall Ports freigeben (Admin)"* erstellt per **einmaligem UAC-Prompt** (`cmd.exe` + `netsh`) eingehend **TCP `48211–48215`** (`"Screen2PC"`) & **UDP `48210`** (`"Screen2PC Discovery"`). Drag&Drop sowohl im Statusfenster als auch direkt in jedem `D3D11Renderer`-Fenster.
  - *Streamfenster (`D3D11Renderer`):* `F11` Vollbild-Toggle, `ESC` verlässt Vollbild, `Ctrl+Alt+K` lokaler KVM-Toggle (`m_suppressKeyUpMask = 0x7`), `Alt+F4` leitet lokalen Fensterschluss (`WM_CLOSE`) durch -> schließt die **gesamte** Session (`StopSession`). `WM_MOUSEMOVE` gedrosselt auf `>= 4 ms` (~250 Hz); `WM_XBUTTON1/2` & `WM_MOUSEHWHEEL` aktiv; `WM_KILLFOCUS` führt `ReleaseCapture()` + Key-Up für `Ctrl/Alt/Shift/LWin` aus.
- **INI-Konfiguration & Pfade:**
  - `%APPDATA%\Screen2PC\sender.ini` (`[Sender]`): `LastIp=""`, `ModeIdx=0`, `CodecIdx=0`, `BitrateIdx=1` (`15/25/40/60`), `ClipboardSync=1`, `KvmEnabled=1` *(Checkboxen sofort, IP/Dropdowns erst bei Connect gespeichert; `targetFps=60` fest)*.
  - `%APPDATA%\Screen2PC\receiver.ini` (`[Receiver]`): `StartFullscreen=1`, `ClipboardSync=1`, `KvmEnabled=1` *(sofort gespeichert)*.
  - `C:\ProgramData\Screen2PC\monitors.cfg`: Virtuelle Monitor-Persistenz für den Treiber.
  - `%USERPROFILE%\Downloads\Screen2PC`: Zielordner für empfangene Dateien.
  - `%TEMP%\Screen2PC_Sender.log`, `%TEMP%\Screen2PC_Receiver.log`, `%TEMP%\Screen2PC_Tests.log` (UTF-8 Append ohne Rotation; `300`-Zeilen-Ringpuffer; GUI zeigt im Leerlauf letzte 6 bzw. 5 Zeilen).

---

## 5. Bekannte technische Eigenheiten & Audit-Hinweise
1. **`Ctrl+Alt+K` vs. Receiver-Checkbox (`KvmEnabled`) Desync:** `Ctrl+Alt+K` schaltet nur `D3D11Renderer::m_kvmEnabled` um, nicht `ReceiverEngine::m_kvmEnabled`, Checkbox oder `receiver.ini`. Startet der Receiver mit `KvmEnabled=0`, schluckt `Ctrl+Alt+K` lokal Eingaben (`[KVM AN]`), während `SendKvmMouse`/`SendKvmKey` alle Pakete verwerfen.
2. **Sender-UI-Blockaderisiko:** Der 500-ms-Telemetry-Timer in `sender/main.cpp` ruft `GetStatus()` -> `VirtualDisplayCtl::IsDriverActive()` (`WaitNamedPipeW(..., 500)`) synchron auf dem UI-Thread auf.
3. **Treiber/Setup:** `Screen2PCMonitorGetDefaultModes` dedupliziert `1600x900`/`1280x720` nicht vollständig (anders als `QueryModes`); `Screen2PC.DriverSetup.exe --uninstall` entfernt nur PnP-Devnodes, belässt aber das OEM-INF-Paket im Windows Driver Store (`pnputil`).
4. **Sender/Receiver Lifecycle-Randfälle:** Schlägt ein `StreamWorker` beim Sender fehl, beendet er sich lokal ohne `m_stop` für `ControlLoop` zu setzen; frühe Handshake-Fehler in `SenderEngine::Connect()` lassen `stateText` bei `"Verbinde mit PC..."` stehen; Receiver akzeptiert nur **1** Session gleichzeitig (`ControlAcceptLoop` blockiert in `HandleControlClient`); `DiscoverResp`-Portfelder werden vom Sender beim Connect ignoriert (`48211` / `48212+i` fest).
5. **Input/UIPI & Hotkey-Details:** `SendInput` am Surface unterliegt Windows-UIPI (ohne Admin-Start keine Steuerung von UAC-/Admin-Fenstern); beim Verlassen des Receiver-Vollbilds via `ESC` entweicht das `ESC`-Key-Up via KVM ans Surface; `F11` (Renderer) und `Ctrl+Shift+K` (Sender) prüfen kein Repeat-Bit / `MOD_NOREPEAT`.
6. **Codec-/Clipboard-/Socket-Grenzen:** Runtime-H.264-Fallback auf Sender (`GPU->CPU->Delta`) und Receiver (`DXVA->CPU`) greift nur solange noch kein einziger Frame erfolgreich en-/dekodiert wurde; `ClipboardSync` kappt lokalen Text bei `2 MB` `GlobalSize` (Remote bis `16 MB`, `CF_UNICODETEXT` vor `CF_DIB`); `UdpSocket::m_sock` ist (anders als `TcpSocket::m_sock`) kein `std::atomic<SOCKET>`.

---

## 6. Changelog
| Datum | Schritt | Änderung |
|---|---|---|
| 2026-11-12 | Init & Planung | `MEMORY.md` angelegt; Hardware-Specs, C++20-Entscheidung, Dual-Codec & treiberfreier PC-Empfänger fixiert |
| 2026-11-12 | Initial Implementation | `Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Solution, Scripts, `ci.yml` & `README.md` erstellt (`a8543e7`) |
| 2026-11-12 | CI, Driver & Codec Fixes | WDK NuGet `10.0.26100.2454` + x64 MSBuild in `ci.yml`; `IddCxStub.lib`, `WDF_DECLARE_CONTEXT_TYPE`, `PnpLockdown=1`, `SignMode=Off` & Post-Link-DLL-Kopie für `Inf2Cat`; `H264Decoder` Macroblock-Padding vs. `MF_MT_MINIMUM_DISPLAY_APERTURE`-Crop; `$RootDir`-Fix in Scripts; Hotkey-Reentry-Guard (`5d07e38`..`43e9140`; CI grün `43330726483`/`43331141875`, 4/4 Tests) |
| 2026-11-13 | Phase 1: Deep Debug & Hardening | `ReceiverEngine` Control-Mutex-Deadlock (`m_ctrlSendMutex` + `WaitReadable`) gefixt; `TcpSocket::SendPacket` Single-Send-Bundling; `D3D11Renderer` `QuadCB` & `WM_PAINT`/`WM_SIZE`-Repaint optimiert; `H264Encoder` Staging-Texture gecacht; `Driver` `AdapterInitFinished`-Race & Cleanup gefixt; `ExtendSecondary` + `SetDisplayConfig`-Extend-Fallback + `sender.ini`/`receiver.ini` + `1080p`/Corrupt/Large-TCP-Tests (`ed7560f`) |
| 2026-11-13 | Phase 2A: Shared Clipboard | `ClipboardSync` (`HWND_MESSAGE` + `AddClipboardFormatListener`, `CF_UNICODETEXT` bis 2 MB + `CF_DIB` bis 16 MB, 64-Bit-FNV-1a Anti-Echo, `OpenClipboard`-Retry) in `Protocol.h`, Engines, UI/INI & Tests integriert (`0bea31f`, CI grün `43332884439`) |
| 2026-11-13 | Phase 2B: Quick File Transfer | `FileTransfer` (TCP `48214` Full-Duplex, 64-KB-Chunks + CRC32, `.part_`-Tempdatei, Path-/Drive-Prefix-Sanitization, Auto-Rename) via `WM_DROPFILES` (Hauptfenster + `D3D11Renderer`), `GetOpenFileNameW`, Ordner-Button & Loopback-Test integriert (`0328efb` + Hunk-Fix `34b9f12`) |
| 2026-11-13 | Phase 3: Pure User-Mode KVM | `KvmInput` (Letterbox-Mapping + Virtual-Desktop `SendInput`), `MsgType::KvmMouse`/`KvmKey`, `D3D11Renderer` Input-Capture + `Ctrl+Alt+K` + `WM_KILLFOCUS`-Modifier-Release, Engines/UI/INI, `SenderEngine::Disconnect`-Join-Fix & `TestKvmInput` (`ffa80dc`, CI grün `43341834757`) |
| 2026-11-13 | Deep Audit & Concurrency Hardening | `TcpSocket::m_sock` atomar; `WM_KILLFOCUS` `ReleaseCapture()`; simultaner Full-Duplex-FileTransfer- & exakter `2560x1440`+`1920x1080`-KVM-Test; `m_workersMutex` (Swap-before-Join) & `m_lifecycleMutex`/`m_disconnectMutex`; getrackte UI-Threads statt `.detach()`; EOF-Spin-Fix; `WaitReadable(10)`-Polling im StreamWorker; einzelner UAC-Firewall-Prompt (`f205ed4`, `1829119`, CI grün `43342527594`/`43343069788`) |
| 2026-11-13 | Hardware MFT & Driver Polish | `ID3D10Multithread::SetMultithreadProtected(TRUE)` in `DxgiCapture`, `D3D11Renderer`, `H264Encoder` & `H264Decoder`; initialer `GPU->CPU->Delta` Fallback im Sender-`StreamWorker`; `Screen2PCMonitorQueryModes` dedupliziert (`3a9d368`, CI grün `43343658228`; Docs `e8390cd` Run `43344128149`) |
| 2026-11-13 | Final Hardening & Audit | `FileTransfer` TCP-EOF-Spin & `active`-Reset gefixt; `SenderEngine` `streamIndex`-Matching (`ExtendSecondary`) + zentrales `CleanupSessionResources()`; `H264Decoder` DXVA->CPU-Fallback (`allowHardware=false`) + 1s-Keyframe-Recovery; `m_suppressKeyUpMask` für `Ctrl+Alt+K`; asynchrones „Trennen“ & joinbarer `g_scanThread`; CPU-Decode- & Peer-EOF-Tests (`1a84fae`, CI Run `43374918851` -> `f442f05`, CI Run `43375193571`, Artifact `14014620658`) |
| 2026-11-13 | Full-Codebase Memory Sync | Komplettes Audit aller 6 Projekte, Scripts & CI gegen `MEMORY.md`: Stand (`1a84fae`/`f442f05`, Run `43375193571`, Artifact `14014620658`), exakten Warning-Status (`Driver` `C4471` & INF `2084`), Codec-/Bitrate-(`15/25/40/60`)/`monitors.cfg`-Details, vollständige `Repository Map`, Operational Reference (Modi/Hotkeys/INI/Firewall/Pfade) & Abschnitt 5 (Audit-Eigenheiten & Testgrenzen) nachgezogen |
