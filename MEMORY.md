# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `Phase 0–3 Komplett & Hardening-Audits CI-verifiziert`
- **Status:** Alle geplanten Kern- & Ausbauphasen (UMDF2 Virtual Display Driver, H.264/DXVA + Delta Lossless Streaming, Shared Clipboard, Full-Duplex Quick File Transfer, Treiberfreies KVM inkl. Sub-10ms-Polling, Multi-Thread-Hardening & Runtime-MFT-Fallback) sind implementiert und in CI Run `43343658228` (`3a9d368`) mit 0 Projekt-Warnings und 7/7 bestandenen Testsuiten gruen verifiziert.
- **Next Step:** Auf `06:45 CET` Credit-Reset-Fenster per Timer warten und danach vor `09:00 CET` die finale Abnahme & Bedienungsanleitung an den User liefern.

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440`, Monitor 2: `1920x1080`.
- **Ziel (Nachtschicht bis 09:00 Uhr):**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), flüssig und latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im Vollbild/Borderless-Fenster dargestellt.
  - Automatischer Windows-Build + Tests via GitHub Actions bei jedem Commit (inkl. fertigem Release-ZIP-Artifact).
  - Ausbau: Bidirektionale Zwischenablage (Text/Bilder), Quick File Transfer (PC <-> Surface) und KVM-Eingabe (PC-Maus/Tastatur -> Surface, treiberfrei).

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (Win32 + D3D11 + DXGI + Media Foundation + Winsock2 + UMDF2 `IddCx`)**
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, spart RAM auf dem 8-GB-Surface, verhindert GC-Microruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen Solution (`Screen2PC.sln`).
2. **PC bleibt komplett treiberfrei (Pure User-Mode):**
   - `Screen2PC.Receiver.exe` nutzt nur Standard-Win32/D3D11/Media Foundation -> keinerlei Konflikt mit Kernel-Anti-Cheat auf dem Gaming-PC. Auch KVM erfasst Eingaben nur ueber normale Fenster-Messages (`WM_*`) des aktiven Streamfensters (kein `SetWindowsHookEx` Global Hook!).
3. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx`):**
   - Minimaler User-Mode-Display-Treiber ohne Kernel-`.sys`.
   - Kommunikation `Sender <-> Driver` über Named Pipe `\\.\pipe\Screen2PC_VDD` (mit `Authenticated Users` ACL) + Fallback `%ProgramData%\Screen2PC\monitors.cfg` (umgeht das bekannte `IndirectKmd.sys` `CreateFile` Access-Denied-Problem nach Reboots).
   - `IddCx` stellt 0..2 virtuelle Monitore exakt in den Auflösungen der PC-Monitore bereit; `Screen2PC.Sender.exe` greift diese sauber via `DXGI Desktop Duplication` ab (inkl. Dirty-Rects & Cursor).
4. **Dual-Codec-Strategie (Ausfallsicher + Maximum Qualität/Speed):**
   - **Standard (`H.264 Low-Latency`):** Hardware-Encoding via Media Foundation MFT (AMD VCN auf Surface) -> Hardware-Decoding (Nvidia RTX 4070 NVDEC auf PC), 20–35 Mbit/s pro Monitor, Software-MFT-Fallback integriert.
   - **Sharp-Modus / Fallback (`Lossless Dirty-Rect Delta`):** Nutzt `DXGI` Dirty-Rects + schnellen Lossless-Tile-Codec (4:4:4 RGB, perfekt scharfer Text für Code/Office, minimale Bandbreite bei statischem Desktop).
5. **Transport:** UDP Broadcast Discovery (`48210`) + TCP Control (`48211`, inkl. `ClipboardSync` & `KvmMouse`/`KvmKey`) + dedizierte `TCP_NODELAY`-Streams pro Monitor (`48212+`) + dedizierter File-Transfer-Kanal (`48214`) mit CRC32-Chunking.

---

## 3. Repository Map
| Pfad | Zweck |
|---|---|
| `MEMORY.md` | Zentrale Planungs-, Architektur- & Changelog-Datei |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props` | VS 2022 Solution + WDK/SDK NuGet-Integration fuer CI |
| `src/common/` | `Protocol.h`, `Logger`, `NetSocket` (UDP/TCP), `CodecH264` (MF GPU+CPU), `CodecDelta` (Lossless RLE), `ClipboardSync` (`CF_UNICODETEXT` + `CF_DIB`), `FileTransfer` (TCP `48214` CRC32 Chunk-Transfer), `KvmInput` (Aspect-Letterbox-Mapping + Virtual-Desktop `SendInput`) |
| `src/driver/` | UMDF v2 `IddCx 1.2` Virtual Display Driver (`Screen2PCDriver.dll`, `Screen2PCDriver.inf`, Named Pipe) |
| `src/driversetup/` | Eigenstaendiges CLI (`Screen2PC.DriverSetup.exe`) fuer SetupAPI/Devnode Install/Remove/Status |
| `src/sender/` | Surface App (`DxgiCapture`, `VirtualDisplayCtl`, `SenderEngine`, Win32 Tray/GUI + `Ctrl+Shift+K`) |
| `src/receiver/` | PC App (`D3D11Renderer` mit Cursor-Overlay, KVM-Capture, Drag&Drop & `F11`/`ESC`/`Ctrl+Alt+K`, `ReceiverEngine`, Win32 Tray/GUI) |
| `src/tests/` | Automatisierte Unit- & Integrationstests (`Screen2PC.Tests.exe`, 7 Testsuiten inkl. Clipboard, Full-Duplex FileTransfer & KVM) |
| `scripts/` | `install_driver.ps1` & `uninstall_driver.ps1` (nur fuer Surface!) |
| `.github/workflows/ci.yml` | Automatischer Windows x64 Build, Testlauf, `infverif`/`Inf2Cat` & Artifact ZIP |

---

## 4. Changelog
| Datum | Schritt | Änderung |
|---|---|---|
| 2026-11-12 | Init | `MEMORY.md` angelegt; Workflow & Architektur-Entwurf erstellt |
| 2026-11-12 | Planung | Hardware-Specs, C++20-Entscheidung, Dual-Codec & treiberfreier PC-Empfänger in `MEMORY.md` fixiert |
| 2026-11-12 | Implementation | `Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Solution, Scripts, `ci.yml` & `README.md` erstellt |
| 2026-11-12 | CI & Driver Fix | WDK NuGet `10.0.26100.2454` + x64 MSBuild in `ci.yml` integriert; `IddCxStub.lib`, `WDF_DECLARE_CONTEXT_TYPE`, `PnpLockdown=1`, `SignMode=Off` und DLL-Kopie fuer `Inf2Cat` im Treiberprojekt gefixt |
| 2026-11-12 | Codec & Scripts | `H264Decoder` um Trennung zwischen Macroblock-Padding (`m_codedHeight`, z. B. `1088` bei `1080p` / `368` bei `360p`) und sichtbarer Aperture (`MF_MT_MINIMUM_DISPLAY_APERTURE`) + DXVA-Crop erweitert; `$RootDir` in `install_driver.ps1`/`uninstall_driver.ps1` fuer entpacktes Release-ZIP gefixt; Hotkey-Reentry-Guard im Sender ergaenzt |
| 2026-11-12 | CI Verifikation | GitHub Actions Run gruen (`43330726483` / `43331141875`): MSBuild `Release|x64`, `Screen2PC.Tests.exe` (4/4 Testsuiten bestanden), `infverif.exe`, `Inf2Cat.exe` & Artifact `Screen2PC-Windows-x64.zip` komplett erfolgreich |
| 2026-11-13 | Phase 1: Deep Debug & Hardening | `ReceiverEngine` Control-Mutex-Deadlock (`m_ctrlSendMutex` + `WaitReadable`) gefixt; `TcpSocket::SendPacket` auf Single-Send gebuendelt; `D3D11Renderer` `m_quadCb` & `WM_PAINT`/`WM_SIZE`-Repaint optimiert; `H264Encoder` Staging-Texture gecached; `Driver` `AdapterInitFinished`-Race & Fehler-Cleanup gefixt; `DisplayMode::ExtendSecondary` (nur PC-Monitor 2) + `SetDisplayConfig`-Extend-Fallback + `sender.ini`/`receiver.ini` Settings-Persistenz + `1080p` H.264/Delta-Corruption/Large-TCP-Tests ergaenzt |
| 2026-11-13 | Phase 2A: Shared Clipboard | `ClipboardSync` (`HWND_MESSAGE` + `AddClipboardFormatListener`, `CF_UNICODETEXT` bis 2 MB + `CF_DIB` bis 16 MB, 64-Bit FNV-1a Anti-Echo, `OpenClipboard`-Retry) erstellt und in `Protocol.h`, `SenderEngine`, `ReceiverEngine`, Sender/Receiver UI (inkl. INI-Persistenz) und Unit-Tests verdrahtet (CI gruen `43332884439`) |
| 2026-11-13 | Phase 2B: Quick File Transfer | `FileTransfer` (`TCP 48214` Full-Duplex, 64-KB-Chunks + CRC32-Pruefung, `.part`-Tempdatei, Path-Traversal-Sanitization, Auto-Rename `datei (1).ext`) erstellt und via `WM_DROPFILES` Drag&Drop (Hauptfenster + `D3D11Renderer`-Fenster), Dateiauswahl-Dialog (`GetOpenFileNameW`), Ordner-Button & Loopback-Test verdrahtet; `SanitizeFilename` Drive-Prefix-Fix ergaenzt |
| 2026-11-13 | Phase 3: Pure User-Mode KVM | `KvmInput` (Aspect-Letterbox-Mapping + Multi-Monitor Virtual Desktop `SendInput`), `MsgType::KvmMouse`/`KvmKey`, `D3D11Renderer` Maus-/Tastaturerfassung + `Ctrl+Alt+K`-Toggle + `WM_KILLFOCUS`-Modifier-Release, `ReceiverEngine`/`SenderEngine` Forwarding & Injektion, Checkboxen/INI in Sender/Receiver, Thread-Join-Race in `SenderEngine::Disconnect` gefixt & `TestKvmInput` ergaenzt (CI gruen `43341834757`) |
| 2026-11-13 | Deep Audit & Hardening | `TcpSocket::m_sock` auf `std::atomic<SOCKET>` fuer race-freies paralleles `Close()`/Shutdown umgestellt; `D3D11Renderer` `WM_KILLFOCUS` um `ReleaseCapture()` ergaenzt; `TestFileTransfer` auf simultanen Full-Duplex-Transfer (Senden+Empfangen gleichzeitig) & `TestKvmInput` auf `2560x1440`+`1920x1080`-Geometrie erweitert (CI gruen `43342527594`) |
| 2026-11-13 | Concurrency & KVM Latency Fix | `SenderEngine`/`ReceiverEngine` `m_workers` via `m_workersMutex` (Swap-before-Join) & `m_lifecycleMutex`/`m_disconnectMutex` vor Datenrennen geschuetzt; getrackte Join-Threads (`g_connectThread`/`g_disconnThread`) statt `.detach()`; TCP-EOF-100%-CPU-Spin (`err == 0` nach `WaitReadable`) gefixt; `RunStreamWorker` auf `WaitReadable(10)` (<10 ms KVM-Reaktionszeit) umgestellt; Firewall-Setup auf einzelnen UAC-Prompt gebuendelt (CI gruen `43343069788`) |
| 2026-11-13 | Hardware MFT & Driver Polish | `ID3D10Multithread::SetMultithreadProtected(TRUE)` in `DxgiCapture`, `H264Encoder` & `H264Decoder` fuer AMD VCN / Nvidia NVDEC DXGI-Device-Sharing aktiviert; automatischer Runtime-Fallback (`GPU H.264 -> CPU H.264 -> Delta Lossless`) bei MFT-Initialisierungsfehlern im StreamWorker; `Driver` `Screen2PCMonitorQueryModes` dedupliziert (CI gruen `43343658228`) |
