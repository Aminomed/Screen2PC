# Screen2PC — Project Memory

> **Regel für Agent & User:** Diese Datei ist die zentrale Gedächtnis- und Planungsdatei.
> Zu Beginn jeder Session **nur diese Datei** lesen. Nach jedem Schritt kompakt aktualisieren (Status, Modul-Index, Changelog).

---

## 0. Current State (Read First!)
- **Phase:** `2 — CI-Verifikation & Feinschliff`
- **Status:** Komplette Codebasis (`Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Scripts, `Screen2PC.sln`, `ci.yml`, `README.md`) erstellt. Starte GitHub Actions Build & Test-Verifikation.
- **Next Step:** Commit + Push -> `gh run watch` -> etwaige Build-/Test-/Driver-Fehler beheben bis CI 100 % gruen ist.

---

## 1. Projektziel & Rahmenbedingungen
- **App-Name:** `Screen2PC` (100 % Eigenbau, keine Third-Party-Apps).
- **Sender (Surface):** Windows 11 (Privatgerät, Admin-Rechte), **AMD Ryzen 5 Microsoft Surface Edition** (x64), **AMD Radeon iGPU**, **8 GB RAM**, WLAN im selben Subnetz.
- **Empfänger (PC):** Windows 11 (Gaming-PC mit Anti-Cheat-Spielen -> **KEIN Treiber / KEIN Test-Signing auf dem PC!**), **Nvidia RTX 4070**, **1 Gbit/s LAN**, Monitor 1: `2560x1440`, Monitor 2: `1920x1080`.
- **Ziel (MVP bis 09:00 Uhr):**
  - 1 bis 2 virtuelle Zusatzmonitore am Surface (passend zu den PC-Monitoren), flüssig und latenzarm über lokales LAN/WLAN auf den PC gestreamt und dort per D3D11 im Vollbild/Borderless-Fenster dargestellt.
  - Automatischer Windows-Build + Tests via GitHub Actions bei jedem Commit (inkl. fertigem Release-ZIP-Artifact).
- **Spätere Roadmap (nach MVP):** Shared Clipboard, Quick File Transfer, KVM (PC-Maus/Tastatur -> Surface).

---

## 2. Architekturentscheidungen & Begründung (Transparent)
1. **Sprache & Stack: Reines C++20 (Win32 + D3D11 + DXGI + Media Foundation + Winsock2 + UMDF2 `IddCx`)**
   - *Warum gegen .NET 8 + C++ Core:* Spart die ~70 MB Runtime / P/Invoke-Interop-Schicht, spart RAM auf dem 8-GB-Surface, verhindert GC-Microruckler und hält App + GPU-Pipeline + Treiber in einer einheitlichen Solution (`Screen2PC.sln`).
2. **PC bleibt komplett treiberfrei (Pure User-Mode):**
   - `Screen2PC.Receiver.exe` nutzt nur Standard-Win32/D3D11/Media Foundation -> keinerlei Konflikt mit Kernel-Anti-Cheat auf dem Gaming-PC.
3. **Virtuelle Monitore auf dem Surface (`Screen2PC.Driver` — UMDF v2 `IddCx`):**
   - Minimaler User-Mode-Display-Treiber ohne Kernel-`.sys`.
   - Kommunikation `Sender <-> Driver` über Named Pipe `\\.\pipe\Screen2PC_VDD` (mit `Authenticated Users` ACL) + Fallback `%ProgramData%\Screen2PC\monitors.cfg` (umgeht das bekannte `IndirectKmd.sys` `CreateFile` Access-Denied-Problem nach Reboots).
   - `IddCx` stellt 0..2 virtuelle Monitore exakt in den Auflösungen der PC-Monitore bereit; `Screen2PC.Sender.exe` greift diese sauber via `DXGI Desktop Duplication` ab (inkl. Dirty-Rects & Cursor).
4. **Dual-Codec-Strategie (Ausfallsicher + Maximum Qualität/Speed):**
   - **Standard (`H.264 Low-Latency`):** Hardware-Encoding via Media Foundation MFT (AMD VCN auf Surface) -> Hardware-Decoding (Nvidia RTX 4070 NVDEC auf PC), 20–35 Mbit/s pro Monitor, Software-MFT-Fallback integriert.
   - **Sharp-Modus / Fallback (`Lossless Dirty-Rect Delta`):** Nutzt `DXGI` Dirty-Rects + schnellen Lossless-Tile-Codec (4:4:4 RGB, perfekt scharfer Text für Code/Office, minimale Bandbreite bei statischem Desktop).
5. **Transport:** UDP Broadcast Discovery (`48210`) + TCP Control (`48211`) + dedizierte `TCP_NODELAY`-Streams pro Monitor (`48212+`) mit Queue-Cap (kein Packet-Loss-Artefakt-Schmieren wie bei ungeschütztem UDP, gleichzeitig keine Bufferbloat-Latenz).

---

## 3. Repository Map
| Pfad | Zweck |
|---|---|
| `MEMORY.md` | Zentrale Planungs-, Architektur- & Changelog-Datei |
| `Screen2PC.sln`, `packages.config`, `Directory.Build.props` | VS 2022 Solution + WDK/SDK NuGet-Integration fuer CI |
| `src/common/` | `Protocol.h`, `Logger`, `NetSocket` (UDP/TCP), `CodecH264` (MF GPU+CPU), `CodecDelta` (Lossless RLE) |
| `src/driver/` | UMDF v2 `IddCx 1.2` Virtual Display Driver (`Screen2PCDriver.dll`, `Screen2PCDriver.inf`, Named Pipe) |
| `src/driversetup/` | Eigenstaendiges CLI (`Screen2PC.DriverSetup.exe`) fuer SetupAPI/Devnode Install/Remove/Status |
| `src/sender/` | Surface App (`DxgiCapture`, `VirtualDisplayCtl`, `SenderEngine`, Win32 Tray/GUI + `Ctrl+Shift+K`) |
| `src/receiver/` | PC App (`D3D11Renderer` mit Cursor-Overlay & `F11`/`ESC`, `ReceiverEngine`, Win32 Tray/GUI) |
| `src/tests/` | Automatisierte Unit- & Integrationstests (`Screen2PC.Tests.exe`) |
| `scripts/` | `install_driver.ps1` & `uninstall_driver.ps1` (nur fuer Surface!) |
| `.github/workflows/ci.yml` | Automatischer Windows x64 Build, Testlauf, `infverif`/`Inf2Cat` & Artifact ZIP |

---

## 4. Changelog
| Datum | Schritt | Änderung |
|---|---|---|
| 2026-11-12 | Init | `MEMORY.md` angelegt; Workflow & Architektur-Entwurf erstellt |
| 2026-11-12 | Planung | Hardware-Specs, C++20-Entscheidung, Dual-Codec & treiberfreier PC-Empfänger in `MEMORY.md` fixiert |
| 2026-11-12 | Implementation | `Common`, `Driver`, `DriverSetup`, `Sender`, `Receiver`, `Tests`, Solution, Scripts, `ci.yml` & `README.md` erstellt |
