# Screen2PC

**Screen2PC** ist eine komplett selbst entwickelte Windows-11-Loesung (100 % C++20, ohne Third-Party-Apps), um die Bildschirme deines **PCs** ueber lokales **LAN / WLAN** als virtuelle Erweiterungsmonitore fuer dein **Surface** zu nutzen — genau wie Miracast *"Wiedergeben"*, nur ueber Ethernet/IP, ohne WLAN-Karte im PC, mit hoher lokaler Bandbreite und ohne Anti-Cheat-Konflikte auf dem Gaming-PC.

---

## Schnellstart

### 1. Auf dem Gaming-PC (Empfaenger — 100 % treiberfrei!)
> **Wichtig:** Auf dem PC wird **kein Treiber** installiert und **kein Test-Signing** aktiviert. Kernel-Anti-Cheat-Spiele bleiben vollstaendig unberuehrt.

1. Entpacke `Screen2PC-Windows-x64.zip` (aus GitHub Actions -> Artifacts).
2. Starte **`Screen2PC.Receiver.exe`**.
3. Klicke einmal auf **"Windows Firewall Ports freigeben (Admin)"** (oeffnet UDP `48210` + TCP `48211–48215` im lokalen Netz).
4. Der Empfaenger erkennt automatisch beide PC-Monitore (z. B. `#1 2560x1440` und `#2 1920x1080`) und wartet im System-Tray.

### 2. Auf dem Surface (Sender)
1. Entpacke `Screen2PC-Windows-x64.zip` (bei Browser-Downloads ggf. ZIP vorher per *Rechtsklick -> Eigenschaften -> Zulassen* entsperren; bei SmartScreen *"Unbekannter Herausgeber"* auf *"Weitere Informationen -> Trotzdem ausfuehren"* klicken — `install_driver.ps1` fuehrt zusaetzlich automatisch `Unblock-File` aus).
2. *(Optional fuer "Erweitern"-Modus; fuer reines Spiegeln nicht noetig)* Starte PowerShell als Administrator und fuehre einmalig aus:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_driver.ps1
   ```
   *(Das Skript installiert das lokale Test-Zertifikat in `Root` + `TrustedPublisher`. Falls Windows Code Integrity trotz Zertifikat Problem 52 meldet, waere `bcdedit /set testsigning on` auf dem Surface noetig, was bei aktivem Secure Boot blockiert ist — alternativ jederzeit treiberfrei mit **Spiegeln** arbeiten.)*
3. Starte **`Screen2PC.Sender.exe`**.
4. Waehle den automatisch gefundenen PC (oder gib die PC-IP ein), waehle Modus (**Erweitern Alle Monitore**, **Erweitern Monitor 1**, **Erweitern Monitor 2** oder **Spiegeln**) sowie Codec:
   - **H.264 Low-Latency:** Nutzt AMD VCN auf dem Surface & Nvidia NVDEC (RTX 4070) auf dem PC (inkl. CPU-MFT-Fallback).
   - **Delta Lossless (4:4:4):** Nutzt DXGI-Dirty-Rects + verlustfreien Tile-RLE-Codec fuer gestochen scharfen Text/Code.
5. Klicke **Verbinden** (oder jederzeit global via **`Ctrl + Shift + K`**).
   - Im Vollbild auf dem PC schaltet **`F11`** / **`ESC`** zwischen randlosem Vollbild und Fenster um.
   - **Geteilte Zwischenablage:** Text & Bilder automatisch zwischen Surface und PC synchronisieren (optional per Checkbox abschaltbar).
   - **Quick File Transfer:** Dateien einfach per **Drag & Drop** ins Hauptfenster oder direkt in ein gestreamtes Monitorfenster ziehen (oder *"Datei(en) senden..."* nutzen); empfangene Dateien landen in `%USERPROFILE%\Downloads\Screen2PC`.
   - **KVM (Maus & Tastatur PC -> Surface):** Steuere das Surface direkt mit PC-Maus und -Tastatur im Streamfenster (umschaltbar per **`Strg + Alt + K`** oder Checkbox; komplett ohne Global Hooks auf dem PC).

---

## Architektur & Transparente Design-Entscheidungen

| Komponente | Ort | Technologie | Grund |
|---|---|---|---|
| `Screen2PC.Driver` (`IddCx 1.2`) | Nur Surface | UMDF v2 User-Mode Display Driver | Erzeugt 0..2 virtuelle Monitore dynamisch in der exakten Aufloesung der PC-Monitore ohne Kernel-`.sys`. Kommunikation via Named Pipe `\\.\pipe\Screen2PC_VDD` (mit `AU`-ACL) + `%ProgramData%\Screen2PC\monitors.cfg` umgeht das bekannte `IndirectKmd` `CreateFile` Access-Denied-Problem. |
| `Screen2PC.DriverSetup` | Nur Surface | SetupAPI / NewDev / CfgMgr32 | Eigenstaendiges CLI zur Treiber-Installation ohne externes `devcon.exe`. |
| `Screen2PC.Sender` | Surface | Win32 + DXGI Desktop Duplication + Media Foundation + Winsock2 | Direkter GPU-Texture-Abgriff mit Dirty-Rects & Hardware-Cursor-Trennung; sauberer Fallback auf Mirror-Modus; `SendInput` im User-Mode fuer KVM. |
| `Screen2PC.Receiver` | Gaming-PC | Win32 + D3D11 + Media Foundation (DXVA/NVDEC) + Winsock2 | Reiner User-Mode-Prozess. Kein Treiber, kein Kernel-Hook (`SetWindowsHookEx`), Anti-Cheat-sicher. |

### Warum reines C++20 statt ".NET 8 + C++ Core"?
- **Vorteile von .NET 8 + C++ Core:** Schnellere GUI-Entwicklung (WPF/WinUI 3, XAML Data-Binding), komfortables Settings-/JSON-Handling.
- **Nachteile von .NET 8 + C++ Core (fuer dieses Projekt ausschlaggebend):** ~70 MB Self-Contained Runtime oder zwingend installiertes .NET 8 Desktop Runtime auf beiden Geraeten, C#/C++ P/Invoke-Marshaling pro Videoframe/Cursor, mehr RAM-Verbrauch auf dem **8-GB-Surface**, moegliche GC-Pausen im Render-Loop und getrennte Toolchains.
- **Vorteile von reinem C++20 (gewaehlter Weg):** Zero-Runtime-Dependencies (kleine, direkt startbare `.exe`s mit `/MT`), direkter Null-Kopie-Zugriff auf DXGI/D3D11/Media Foundation/IddCx, minimaler RAM-Fussabdruck und eine einzige einheitliche `Screen2PC.sln` (inkl. Treiber & Tests).
- **Nachteil von reinem C++20:** Die Win32-GUI ist funktional und schlichter als eine moderne XAML-Oberflaeche (was fuer ein Tray-Utility aber ideal und extrem ressourcensparend ist).

---

## Ports & Sicherheit
- **UDP `48210`:** Lokale LAN-Broadcast-Erkennung (Auto-Discovery)
- **TCP `48211`:** Control-, Clipboard- (`ClipboardSync`), KVM- (`KvmMouse`/`KvmKey`) & Heartbeat-Kanal
- **TCP `48212` / `48213`:** Dedizierte `TCP_NODELAY`-Videostreams pro Monitor (Monitor 1 & Monitor 2)
- **TCP `48214`:** Dedizierter bidirektionaler Dateitransfer-Kanal mit 64-KB-Chunks & CRC32-Pruefsumme
