# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `Hardware-Setup verifiziert — Full-Desktop KVM Roaming, Disconnect-Crash-Fix, 60+/120Hz- & Bitrate-Upgrade (25–200 Mbit/s)`
- **Status:** Der UMDF-v2-Treiber-Fix (`f92af3f`, CI Run `43422111904`) wurde auf echter Hardware (`Surface` + `Gaming-PC` mit `2560x1440` + `1920x1080`) erfolgreich getestet. Darauf aufbauend wurden alle 3 gemeldeten Bugs/UX-Probleme gelöst und im grünen CI-Build **Run `43435633945`** (Commit `65dfd15`, Artifact `14043842203`, 7/7 Tests PASS, INF/CAT OK) bereitgestellt: Disconnect-Crash-Fix (`ClipboardSync`/`FileTransfer`/`SenderEngine`), `WM_INPUT` Full-Desktop KVM-Roaming inkl. physischem Surface-Display & asymmetrischen Monitorübergängen (`StepVirtualDesktopPoint`), sowie 30-Hz-Sleep-Bug/MFT-Async-Wait/`timeBeginPeriod(1)`/`25..200 Mbit/s`. Das vom User erwähnte **HDR-Farbproblem** wurde auf ausdrücklichen Wunsch hier **nur vorgemerkt** (noch nicht umgesetzt).
- **Compiler-, INF- & Test-Realität:**
  - *Build & INF:* Windows-x64 Release-Build, `infverif.exe /v`, `Inf2Cat.exe` & `Screen2PC.Tests.exe` laufen via `.github/workflows/ci.yml` (`windows-2022`).
  - *Test-Scope:* `Screen2PC.Tests.exe` prüft 7 Suiten inkl. `TestKvmInput::StepVirtualDesktopPoint` (3-Monitor-Roaming Surface + 2K + 1080p, Seam-Snapping & vertikale Stacks); reale Win32-UI, `WM_INPUT`/`ClipCursor`, `DxgiCapture`, `D3D11Renderer`, UMDF2-Treiber & Hardware-MFT laufen nur auf realer Windows-Hardware.
- **Next Step:** Neues CI-Bundle `Screen2PC-Windows-x64.zip` (Run `43435633945`) auf **PC** und **Surface** testen (Maus über beide PC-Monitore + zurück aufs Surface schieben, `ESC`/`Strg+Alt+K`, `Trennen -> Verbinden`, `80–200 Mbit/s` & FPS/Latenz prüfen). **HDR-Farbdifferenz erst auf expliziten User-Zuruf umsetzen.**

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440`, Monitor 2: `1920x1080` (ein Monitor nutzt HDR -> siehe Abschnitt 5).
- **Kernfunktionen:**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im randlosen Vollbild (`WS_POPUP`) oder Fenstermodus (`WS_OVERLAPPEDWINDOW`) dargestellt.
  - Automatischer Windows-x64-Release-Build + Tests + INF/CAT-Prüfung via GitHub Actions bei jedem Push/PR (inkl. `Screen2PC-Windows-x64.zip`).
  - Bidirektionale Zwischenablage (Text/Bilder), Full-Duplex Quick File Transfer (PC <-> Surface) und nahtloses Full-Desktop-KVM (PC-Maus/Tastatur steuert virtuelle PC-Displays **und** physischen Surface-Screen ohne PC-Treiber/Hooks).

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (`stdcpp20`, `x64`-only, `/MT` Release / `/MTd` Debug, VS 2022 `v143`)**
   - *Stack:* Win32 + D3D11 + DXGI 1.2 + Media Foundation + Winsock2 + `winmm` (`timeBeginPeriod(1)`) + UMDF v2 (`2.15`, `IddCx 1.2`), `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`.
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, schont RAM auf dem 8-GB-Surface, verhindert GC-Mikroruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen `Screen2PC.sln`.
2. **PC bleibt komplett treiberfrei (Pure User-Mode KVM mit Full-Desktop Roaming):**
   - `Screen2PC.Receiver.exe` installiert keinen Treiber und nutzt kein `SetWindowsHookEx`.
   - **`D3D11Renderer` KVM-Capture (`RegisterRawInputDevices` mit `dwFlags = 0, hwndTarget = nullptr` + `ClipCursor` + `SetCursor(nullptr)`):**
     - Im aktivierten KVM-Modus wird bei Klick ins Bild (oder Vollbild-Fokus) einmalig die Startposition ans Surface gewarpt (`isRelative = 0`) und danach werden ungedrosselte HID-Relativdeltas (`WM_INPUT` `RIM_TYPEMOUSE`, `isRelative = 1` in `KvmMouseEvent::pad[0]`, koalesziert pro Render-/Pump-Tick) ans Surface gesendet, während der lokale PC-Cursor per `ClipCursor` im Client-Rechteck gehalten und per `SetCursor(nullptr)` unsichtbar geschaltet ist (kein „Doppel-Cursor“ / Nachziehen!).
     - Auf dem Surface rechnet `KvmInput::StepVirtualDesktopPoint` die Pixel-Deltas über alle physischen + virtuellen Monitore (`EnumDisplayMonitors`, 1s-Cache) inkl. nahtlosem Border-Snapping bei ungleichen Höhen (`1504` / `1440` / `1080`). Damit wandert der Surface-Mauszeiger frei zwischen Display 1, Display 2 und dem **physischen Surface-Bildschirm** (inkl. Drag & Drop von Fenstern!).
     - `ESC` (verlässt Vollbild + löst Capture), `Ctrl+Alt+K` (synchronisiert `D3D11Renderer`, `ReceiverEngine::m_kvmEnabled` und Checkbox via `KvmToggleCallback`), `WM_ACTIVATE(WA_INACTIVE)` und `WM_KILLFOCUS` geben `ClipCursor` und lokalen Cursor sofort wieder frei.
3. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx 1.2`):**
   - Minimaler User-Mode-Display-Treiber ohne eigenes Kernel-`.sys` (`Screen2PCDriver.inf` mit `ClassVer = 2.0`, `HardwareID = Root\Screen2PCVDD`, `PnpLockdown = 1`, `MaxMonitorsSupported = 2`, `UmdfExtensions = IddCx0102` in `[Screen2PCDriver_Install_UMDF]`, ohne `ProcessSharingDisabled`-Konflikt; Win10 `17763` über `AddService = WUDFRd` und Win11 `22000+` über `Include/Needs = WUDFRD.inf`).
   - *IPC:* Named Pipe `\\.\pipe\Screen2PC_VDD` (SDDL `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGW;;;AU)`, `nMaxInstances = 4` mit sequenziellem Connect-Loop, `SanitizeSpec` klemmt auf `640..7680 x 480..4320 @ 30..240 Hz` bzw. `1920x1080@60`).
   - *Persistenz (Boot/D0-Recovery):* `VirtualDisplayCtl::SetMonitors()` schreibt den `VddCommand` vor dem Pipe-Call hart nach `C:\ProgramData\Screen2PC\monitors.cfg` (bei `ClearMonitors()` / `uninstall_driver.ps1` gelöscht; ACL `*S-1-5-11:(OI)(CI)M` & `*S-1-5-19:(OI)(CI)RX`).
4. **Dual-Codec & Low-Latency Pipeline:**
   - **Standard (`CodecType::H264`; `Auto` mappt auf `H264`):** Low-Latency Media Foundation (`MF_LOW_LATENCY`, `CODECAPI_AVLowLatencyMode`, GOP = `fps`, 0 B-Frames, Mindestbitrate `2 Mbit/s`, gerade Dimensionen `w & ~1`, `h & ~1`).
     - *Sender:* Dropdown-Presets **`25 / 50 / 80 (Default) / 120 / 200 Mbit/s`**; Worker-`targetFps` dynamisch bis `min(refreshHz, 120)`. Kurzes `AcquireNextFrame(4..8 ms)` **ohne** künstlichen Post-Frame-`Sleep`, sodass Cursor-Updates und neue Frames sofort gesendet werden. `H264Encoder` cached `ID3D11VideoProcessorInputView` (`m_vpInView`) und nutzt im Async-MFT-Loop `MF_EVENT_FLAG_NO_WAIT`, ohne vor `METransformHaveOutput` des aktuellen Frames abzubrechen. Worker-Threads initialisieren COM MTA via `ComScope`.
     - *Receiver:* `timeBeginPeriod(1)`, `WaitReadable(needPresent ? 0 : 1)` und Paket-Drain (`<= 8` gepufferte Pakete pro Tick) vor `renderer.Present()` (`Present(0, 0)`). Hardware DXVA-Fallback auf CPU (`allowHardware = false`) bleibt erhalten.
   - **Sharp-Modus (`CodecType::DeltaLossless`):** Verlustfreier **32-Bit-BGRA**-Codec (`DLT1` Magic).
5. **Transport & Wire-Protokoll (`Protocol.h`, `NetSocket`):**
   - IPv4-only (`AF_INET`), Packet-Magic `S2P1` (`0x53325031`), Version `1`, `kMaxFrameBytes = 64 MB`, **ohne TLS/Auth**.
   - `KvmMouseEvent`: `pad[0]` als `isRelative` (`0` = normalisierte Coords auf `monitorIndex`, `1` = rohe Pixel-Deltas auf dem gesamten Virtual Desktop).
   - **Lifecycle-Sicherheit (Disconnect-Crash behoben):** `ClipboardSync::m_lifecycleMutex` (`m_hwnd`/`m_threadId` atomar, Move-before-Join) und `FileTransfer::m_lifecycleMutex` (Move-before-Join) verhindern `std::terminate()` durch konkurrierendes `Stop()`/`Disconnect()`. `SenderEngine::GetStatus()` pollt `IsDriverActive()` maximal alle `2500 ms` und **nie** während Connect/Stream/Disconnect (`!m_connecting && !m_connected && !m_disconnecting`). `OnToggleConnect()` hat atomaren `g_connectBusy`-Guard.

---

## 3. Repository Map
| Pfad | Inhalt & Zweck |
|---|---|
| `MEMORY.md`, `README.md` | Zentrale Projekt-Memory (`MEMORY.md`) & Enduser-Schnellstart (`README.md`) |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props`, `.gitignore` | VS 2022 Solution (6 Projekte, `Debug\|x64` & `Release\|x64`), NuGet-WDK/-SDK `10.0.26100.2454` für `Screen2PC.Driver`, Ignore-Regeln (`out/`, `dist/`, `packages/`) |
| `src/common/` (`Screen2PC.Common.vcxproj`) | `Protocol.h`, `Logger.h/.cpp` (`300`-Zeilen-Ringpuffer), `NetSocket.h/.cpp`, `CodecH264.h/.cpp`, `CodecDelta.h/.cpp`, `ClipboardSync.h/.cpp` (`m_lifecycleMutex`, atomare Handles), `FileTransfer.h/.cpp` (`m_lifecycleMutex`, Full-Duplex), `KvmInput.h/.cpp` (`StepVirtualDesktopPoint`, Multi-Monitor-Virtual-Desktop-Mapping) |
| `src/driver/` (`Screen2PC.Driver.vcxproj`) | `DriverProtocol.h`, `Driver.h/.cpp` (UMDF v2 `IddCx 1.2`), `Screen2PCDriver.inf` |
| `src/driversetup/` (`Screen2PC.DriverSetup.vcxproj`) | `main.cpp` -> `Screen2PC.DriverSetup.exe` |
| `src/sender/` (`Screen2PC.Sender.vcxproj`) | Surface-App: `SenderEngine.h/.cpp`, `DxgiCapture.h/.cpp`, `VirtualDisplayCtl.h/.cpp`, `main.cpp` (`winmm` `timeBeginPeriod(1)`, `25..200 Mbit/s`, `g_connectBusy`) |
| `src/receiver/` (`Screen2PC.Receiver.vcxproj`) | PC-App: `ReceiverEngine.h/.cpp`, `D3D11Renderer.h/.cpp` (`WM_INPUT`, `ClipCursor`, `SetCursor(nullptr)`, `KvmToggleCallback`, `SW_SHOWNOACTIVATE` für Sekundärmonitor), `main.cpp` (`winmm` `timeBeginPeriod(1)`, KVM-Checkbox-Sync) |
| `src/tests/` (`Screen2PC.Tests.vcxproj`) | `main.cpp` -> `Screen2PC.Tests.exe`: 7 Suiten inkl. `TestKvmInput::StepVirtualDesktopPoint` |
| `scripts/` | `install_driver.ps1`, `uninstall_driver.ps1` |
| `.github/workflows/ci.yml` | Windows-2022 CI: MSBuild `Release\|x64`, Tests, `infverif`, `Inf2Cat`, Artifact `Screen2PC-Windows-x64.zip` |

---

## 4. Konfiguration, Hotkeys, Firewall & Pfade
- **Sender Display-Modi (`DisplayMode`):** `ExtendAll` (`0`), `ExtendPrimary` (`1`), `MirrorPrimary` (`2`), `ExtendSecondary` (`3`).
- **Hotkeys & Bedienung:**
  - *Sender (`530x564`):* Globaler Hotkey `Ctrl+Shift+K` (Verbinden/Trennen); `X` minimiert ins Tray.
  - *Receiver (`570x474`):* `X` versteckt ins Tray; Button *"Windows Firewall Ports freigeben (Admin)"*.
  - *Streamfenster (`D3D11Renderer`):* `F11` Vollbild-Toggle; `ESC` verlässt Vollbild **und** gibt KVM-Mausfokus frei (`m_escReleasedCapture = true`); Klick ins Bild (oder Vollbild-Fokus) aktiviert Full-Desktop-KVM-Capture (`WM_INPUT` + `ClipCursor`); `Ctrl+Alt+K` schaltet KVM global im Receiver um (inkl. GUI-Checkbox & Modifier-Key-Up-Suppression `m_suppressKeyUpMask = 0x7`); `Alt+F4` beendet Session (`StopSession`).
- **INI-Konfiguration & Pfade:**
  - `%APPDATA%\Screen2PC\sender.ini` (`[Sender]`): `LastIp=""`, `ModeIdx=0`, `CodecIdx=0`, `BitrateIdx=2` (`25/50/80/120/200`), `ClipboardSync=1`, `KvmEnabled=1`.
  - `%APPDATA%\Screen2PC\receiver.ini` (`[Receiver]`): `StartFullscreen=1`, `ClipboardSync=1`, `KvmEnabled=1`.
  - `C:\ProgramData\Screen2PC\monitors.cfg`, `%USERPROFILE%\Downloads\Screen2PC`, `%TEMP%\Screen2PC_*.log`.

---

## 5. Bekannte technische Eigenheiten & Vorgemerkte Themen
1. **[VORGEMERKT / NICHT UMSETZEN BIS USER ES SAGT] HDR-Farbunterschied zwischen den PC-Monitoren:**
   - *User-Meldung (2026-11-13):* Einer der beiden PC-Monitore nutzt **HDR**, wodurch zwischen den beiden Bildschirmen ein deutlicher Farbunterschied auftritt. Der User bat ausdrücklich: *"merke dir das erstmal, setze das jetzt noch nicht um, erst wenn ich es dir sage."*
   - *Technische Einordnung:* Sowohl `Screen2PC.Driver` / `DxgiCapture` als auch `H264Encoder` / `H264Decoder` / `D3D11Renderer`-SwapChain arbeiten aktuell rein in 8-Bit SDR (`DXGI_FORMAT_B8G8R8A8_UNORM`, BT.709). Auf einem Windows-11-HDR-Monitor (ggf. mit Auto Color Management / WCG) stellt der SDR-SwapChain-Output bzw. abweichende Helligkeits-/Gamma-/Farbraum-Interpretation (`G22` vs `G2084` / SDR Content Brightness) Farben stark abweichend dar. Spätere Ansätze erst nach User-Freigabe evaluieren (z. B. `IDXGISwapChain3::SetColorSpace1`, SDR-Colorimetry-/Brightness-Compensation oder Display-Profil-Hinweis).
2. **Receiver-Session-Limits:** Receiver akzeptiert nur **1** aktive Session gleichzeitig; `DiscoverResp`-Portfelder werden vom Sender beim Connect ignoriert (`48211` / `48212+i` fest).
3. **Input/UIPI & Hotkey-Details:** `SendInput` am Surface unterliegt Windows-UIPI (ohne Admin-Start keine Steuerung von UAC-/Admin-Fenstern); beim Verlassen via `ESC` wird `m_suppressEscUp` lokal abgefangen. `F11` prüft Repeat-Bit; `Ctrl+Shift+K` (`RegisterHotKey`) könnte künftig `MOD_NOREPEAT` nutzen.
4. **Codec-/Clipboard-/Socket-Grenzen:** Runtime-H.264-Fallback auf Sender (`GPU->CPU->Delta`) und Receiver (`DXVA->CPU`) greift nur vor dem ersten erfolgreich dekodierten Frame; `ClipboardSync` kappt Text bei `2 MB` (`CF_DIB` bis `16 MB`); `UdpSocket::m_sock` ist kein `std::atomic<SOCKET>`.

---

## 6. Changelog
| Datum | Schritt | Änderung |
|---|---|---|
| 2026-11-12 | Init & Planung | `MEMORY.md` angelegt; Hardware-Specs, C++20-Entscheidung, Dual-Codec & treiberfreier PC-Empfänger fixiert |
| 2026-11-12 | Initial Implementation | `Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Solution, Scripts, `ci.yml` & `README.md` erstellt (`a8543e7`) |
| 2026-11-12 | CI, Driver & Codec Fixes | WDK NuGet `10.0.26100.2454` + MSBuild in `ci.yml`; `H264Decoder` Crop-Fix; Test-Suites (`5d07e38`..`43e9140`) |
| 2026-11-13 | Phase 1–3 & Hardening | `ClipboardSync` (`0bea31f`), `FileTransfer` (`0328efb`), KVM (`ffa80dc`), Concurrency-/Fallback-Hardening (`f205ed4`..`87f0a7e`) |
| 2026-11-13 | INF Problem-31 Fix (Hardware OK) | `UmdfExtensions = IddCx0102`, Win11 `WUDFRD.inf`, `.cat`-only Signierung & `oem*.inf`-Purge (`f92af3f`, Run `43422111904`) — auf Surface + PC mit 2 Monitoren erfolgreich verifiziert |
| 2026-11-13 | Full-Desktop KVM, Disconnect-Fix & High-FPS/Bitrate | `WM_INPUT` + `ClipCursor` + `SetCursor(nullptr)` + `StepVirtualDesktopPoint` (freies Maus-Roaming über beide PC-Monitore & physisches Surface-Display inkl. Edge-Crossing-Fix `65dfd15`, `ESC` löst Fokus, `Ctrl+Alt+K` mit Checkbox-Sync); Disconnect-Crash (`m_lifecycleMutex` + Move-before-Join in `ClipboardSync`/`FileTransfer`/`SenderEngine` + `m_disconnecting` + idle Pipe-Cache) behoben; 30-Hz-Sleep-Bug & Async-MFT-Stall behoben + `timeBeginPeriod(1)` + Packet-Drain + Bitrates `25/50/80/120/200 Mbit/s`; HDR-Farbunterschied vorgemerkt (Run `43435633945`, Artifact `14043842203`) |
