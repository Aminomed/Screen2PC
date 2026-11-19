<div align="center">

<img src="src/common/assets/Screen2PC.ico" width="72" height="72" alt="Screen2PC Logo" />

# Screen2PC

**Nutze die Monitore, das HDR-Display sowie Maus & Tastatur deines Gaming-PCs als kabellose, extrem latenzarme Erweiterung für deinen Windows-Laptop — 100 % lokal im LAN, ohne Cloud, ohne PC-Treiber.**

**Sprache:** [English](README.md) | **Deutsch**

[![Build & Test](https://github.com/Aminomed/Screen2PC/actions/workflows/ci.yml/badge.svg)](https://github.com/Aminomed/Screen2PC/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/Aminomed/Screen2PC?label=Download)](https://github.com/Aminomed/Screen2PC/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-0078D6)
![Language](https://img.shields.io/badge/C%2B%2B-20%20(Zero%20Runtimes)-orange)

</div>

---

## Warum Screen2PC?

Windows *Drahtlose Anzeige / Miracast* kämpft oft mit Wi-Fi-Direct-Abbrüchen, 30/60-Hz-Limits, fehlendem Multi-Monitor-HDR und spürbarem Input-Lag. **Screen2PC** löst das direkt über dein vorhandenes Heimnetz (WLAN / Gigabit-LAN):

* **Dein Gaming-PC bleibt 100 % treiberfrei & Anti-Cheat-sicher:** `Screen2PC.Receiver.exe` läuft komplett im User-Mode. Es werden **keine** Kernel-/UMDF-Treiber, **keine** globalen Hooks (`SetWindowsHookEx`) und **kein** Test-Signing auf dem PC installiert.
* **Dein Laptop erhält 1–2 echte virtuelle Monitore (`IddCx 1.2`):** Windows erkennt die PC-Monitore (z. B. `2560×1440 @ 170 Hz HDR` + `1920×1080 @ 75 Hz SDR`) als native erweiterte Bildschirme — oder spiegelt das Laptop-Display komplett treiberfrei.
* **Ressourcensparendes reines C++20:** Kein Electron, keine .NET-Runtime, kein WebView2 (~2–5 MB RAM, Sofortstart, keine GC-Mikroruckler).

---

## Screenshots

| PC Receiver — Übersicht & Live-Telemetrie | Laptop Sender — Verbindung & Live-Qualität |
| :---: | :---: |
| ![Receiver Overview](docs/images/receiver-overview.png) | ![Sender Connection](docs/images/sender-connection.png) |

| PC Receiver — Drag & Drop Datei-/Ordner-Transfer | Laptop Sender — Energie, Zuklappen-Guard & KVM |
| :---: | :---: |
| ![Receiver Transfer](docs/images/receiver-transfer.png) | ![Sender Settings](docs/images/sender-settings.png) |

| Permanentes In-Stream-Quick-HUD (`FPS \| Latenz \| Mbit/s \| KVM`) | PC Receiver — Einstellungen & Overlay-Schalter |
| :---: | :---: |
| ![In-Stream Quick-HUD](docs/images/stream-hud.png) | ![Receiver Settings](docs/images/receiver-settings.png) |

---

## Alle Features im Überblick

* **1 bis 2 virtuelle Monitore (`IddCx 1.2`) + Live-Moduswechsel:** Erweitern auf alle PC-Monitore, nur Monitor 1, nur Monitor 2 oder Laptop-Display spiegeln — auch während laufender Verbindung umschaltbar.
* **Dual-Codec-Pipeline (live umschaltbar):**
  * **H.264 Low-Latency (`AMF ULL` / `QuickSync` / `NVDEC Direct`):** Constrained Baseline, 0 B-Frames, 0-Frame-DPB-Delay und dynamisches ABR-Backoff (`15–200 Mbit/s`, `Auto / 60 / 90 / 120 FPS`).
  * **Delta Lossless (`DLT2`, RGB 4:4:4):** Schnelles XOR + LZ4 + DXGI `MoveRects` mit automatischem **100 % verlustfreiem Settle** nach `120 ms` Stillstand für gestochen scharfen Text.
* **Echtes `scRGB`-HDR & Pro-Monitor-Farbkalibrierung:** 16-Bit-Floating-Point `scRGB` mit `DISPLAYCONFIG_SDR_WHITE_LEVEL`-Abgleich + Live-Reglern für Helligkeit, Gamma, Kontrast, Sättigung & RGB-Gain.
* **Treiberfreies 1000-Hz-KVM:** Nahtlose Maus- & Tastatursteuerung über Laptop + virtuelle Monitore per Raw Input (`WM_INPUT` + `ClipCursor`), unabhängige **KVM-Maus- & Scroll-Tempo-Regler** (`25–500 %`) und `Strg+Alt+K`.
* **In-Stream-Quick-HUD:** Kompakte D3D11-Statusleiste oben im Streamfenster mit **FPS, Decode-Latenz (ms), Bitrate (Mbit/s), Codec und KVM-Status**, Hover-Buttons (`KVM`, `Vollbild/Fenster`, `Ausblenden`) und 1-Klick-Schalter in den Einstellungen (`Strg+Alt+H`).
* **Geteilte Zwischenablage & rekursiver Ordner-Transfer:** Synchronisiert Text & Bilder (`CF_DIB`) und überträgt Dateien oder ganze Ordnerstrukturen per Drag & Drop mit **Gesamt-Fortschritt (%), übertragenen/verbleibenden MB, Speed, Abbruch & Abschlussmeldung**.
* **Laptop-Zuklappen-Schutz & Abdunkelung:** Pausiert optional das Windows-Standby beim Zuklappen (`PowrProf` + crash-sicheres INI-Restore) und dunkelt physische Laptop-Displays auf Wunsch ab (`ESC` / Doppelklick zum Reaktivieren).
* **1-Klick-GitHub-Auto-Updater & Zweisprachigkeit:** Prüft automatisch auf GitHub Releases, aktualisiert atomar und unterstützt **Auto / Deutsch / English** direkt in den Einstellungen.

---

## Schnellstart

1. **Download:** Lade `Screen2PC-Windows-x64.zip` aus dem [aktuellen Release](https://github.com/Aminomed/Screen2PC/releases/latest) herunter und entpacke das Archiv auf **PC** und **Laptop**.
2. **Auf dem Gaming-PC (Receiver — keine Installation nötig):**
   * Starte `Screen2PC.Receiver.exe`.
   * *(Optional)* Klicke einmal unter Einstellungen auf **Firewall-Ports öffnen (Admin)**, damit UDP `48210, 48215` und TCP `48211–48215` im privaten LAN erlaubt sind.
3. **Auf dem Laptop (Sender):**
   * *(Einmalig für erweiterte Monitore)* Rechtsklick auf `scripts\install_driver.ps1` → **Mit PowerShell ausführen** (Admin). Dadurch wird der UMDF-v2-Virtual-Display-Treiber `Screen2PC.Driver` mit lokalem selbstsignierten `.cat`-Zertifikat installiert (**kein Windows-Test-Signing nötig**).
   * Starte `Screen2PC.Sender.exe`, wähle deinen PC unter **PC im LAN** und klicke **Verbinden (`Ctrl+Shift+K`)**.
4. **Erste Kopplung:** Gib die 6-stellige PIN vom PC-Bildschirm am Laptop ein. Danach verbinden sich beide Geräte automatisch ohne erneute PIN.

> **Hinweis zu Windows SmartScreen:** Da Screen2PC ein unabhängiges Open-Source-Projekt ohne teures EV-Code-Signing-Zertifikat ist, kann beim ersten Start eine SmartScreen-Warnung erscheinen (*„Weitere Informationen → Trotzdem ausführen“*). Alternativ lässt sich alles jederzeit direkt aus dem Quellcode über Visual Studio 2022 / GitHub Actions selbst bauen.

---

## Sicherheit & Datenschutz

* **100 % lokales Netz:** Bilddaten, KVM-Eingaben, Zwischenablage und Dateien verlassen niemals dein lokales LAN/WLAN. Die einzige optionale Internet-Anfrage ist die Update-Prüfung gegen GitHub Releases (in den Einstellungen abschaltbar).
* **Kryptographische 6-stellige PIN + MAC-Merkliste:** Unbekannte Sender müssen eine per `BCryptGenRandom` erzeugte PIN eingeben (`Pairing.cpp`, Constant-Time-Prüfung). Eine laufende Session wird erst nach bestandener Authentifizierung abgelöst.
* **Path-Traversal-Schutz:** Alle relativen Dateipfade werden bereinigt (`SanitizeRelativePath`), `..` / UNC / Laufwerkspfade werden blockiert; Downloads landen ausschließlich in `%USERPROFILE%\Downloads\Screen2PC`.
* **Saubere Deinstallation:** Mit `scripts\uninstall_driver.ps1` (Admin) lässt sich der virtuelle Monitor-Treiber am Laptop rückstandslos entfernen.

---

<details>
<summary><strong>Under the Hood — Architektur & Low-Latency-Technik (Geek-Deep-Dive)</strong></summary>

### Stack & Ressourcenbedarf
* **Sprache:** Reines **C++20** (`stdcpp20`, `x64`, `/MT` Static-CRT, Visual Studio 2022 `v143`) — null Drittanbieter-Runtimes.
* **APIs:** Win32 (`Per-Monitor DPI v2`, `Common Controls v6`), D3D11, DXGI 1.6 (`IDXGIOutput6`, `ALLOW_TEARING`, Waitable SwapChain), Media Foundation, Winsock2, `IPHelper`, `BCrypt`, `PowrProf`, WinHTTP, UMDF v2 (`IddCx 1.2`).

### Netzwerk-Ports (`src/common/Protocol.h`)
| Port | Protokoll | Zweck |
| :--- | :--- | :--- |
| `48210` | UDP | LAN-Discovery (`DiscoverReq` / `DiscoverResp` / `VersionMismatch`) |
| `48211` | TCP | Control-Kanal (Auth, `ReconfigureStream`, `ClipboardSync`, zuverlässiger KVM-Fallback, Heartbeat) |
| `48212–48213` | TCP | Videostream pro Monitor + Cursor-Shapes (asymmetrische Socket-Puffer: `256K/64K` Sender, `64K/512K` Receiver) |
| `48214` | TCP | Dedizierter rekursiver Datei-/Ordner-Transfer (`64 KB`-Chunks, CRC32-geprüft) |
| `48215` | UDP | **Realtime-KVM- & Cursor-Kanal** (`THREAD_PRIORITY_HIGHEST`, `kMouseMove` UDP-only, Tasten/Scroll per Dual UDP+TCP dedupliziert) |

### Low-Latency-Techniken
1. **Sub-Millisekunden-Maus (`WM_INPUT` + UDP):** Rohe HID-Mausdeltas werden ohne Timer-Batching sofort (`<= 1 ms` / 1000 Hz) über UDP `48215` gesendet, inkl. Sub-Pixel-Akkumulatoren (`KvmInput`).
2. **Waitable `FLIP_DISCARD` + `ALLOW_TEARING`:** `D3D11Renderer` nutzt eine `MsgWaitForMultipleObjectsEx`-Schleife direkt auf `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (MaxFrameLatency `1`), entkoppelt vom `recvDecodeThread`.
3. **Zero-Copy DXVA `NV12` → `scRGB` Shader:** Dekodierte H.264-`NV12`-Surfaces werden direkt auf der GPU kopiert (`CopySubresourceRegion`) und in `PSMainNv12` (BT.709 Studio → `scRGB` HDR/SDR) gewandelt — ohne `VideoProcessorBlt`.
4. **Latest-Frame-Wins Sender-Queue:** `SenderEngine` konvertiert über einen 4-Slot-GPU-NV12-Ring und verwirft veraltete Frames vor dem TCP-Send (bei übersprungenem H.264-Delta wird automatisch ein IDR-Keyframe angefordert).
5. **In-Stream Quick-HUD:** Als gecachte premultiplied GDI-DIB → D3D11-Textur direkt in `D3D11Renderer::Present()` gerendert, sodass DWM im Hardware-Independent-Flip (`ALLOW_TEARING`) bleibt.

</details>

---

## Selbst kompilieren

* **Voraussetzungen:** Visual Studio 2022 (`v143` C++20), Windows 11 SDK + WDK NuGet (`10.0.26100.2454` über `packages.config`).
* `Screen2PC.sln` öffnen, NuGet-Pakete wiederherstellen und **`Release | x64`** bauen.
* `out\Release\x64\Screen2PC.Tests.exe` führt die automatisierten Unit- & Integrationstests aus.

---

## Lizenz

Veröffentlicht unter der [MIT-Lizenz](LICENSE).
