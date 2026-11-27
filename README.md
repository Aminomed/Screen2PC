<div align="center">

<img src="src/common/assets/Screen2PC.ico" width="72" height="72" alt="Screen2PC Logo" />

# Screen2PC

**Turn your Gaming PC's monitors, HDR display, mouse & keyboard into an ultra-low-latency wireless extension for your Windows Laptop — 100% local LAN, zero cloud, zero PC drivers.**

**Language:** **English** | [Deutsch](README.de.md)

[![Build & Test](https://github.com/Aminomed/Screen2PC/actions/workflows/ci.yml/badge.svg)](https://github.com/Aminomed/Screen2PC/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/Aminomed/Screen2PC?label=Download)](https://github.com/Aminomed/Screen2PC/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-0078D6)
![Language](https://img.shields.io/badge/C%2B%2B-20%20(Zero%20Runtimes)-orange)

</div>

---

## Why Screen2PC?

Windows *Wireless Display / Miracast* often suffers from Wi-Fi Direct connection drops, 30/60 Hz caps, no multi-monitor HDR support, and noticeable input lag. **Screen2PC** solves this over your existing local network (Wi-Fi / Gigabit LAN):

* **Your PC stays 100% driver-free & anti-cheat safe:** `Screen2PC.Receiver.exe` runs purely in User-Mode. It installs **zero** kernel/UMDF drivers, **zero** global hooks (`SetWindowsHookEx`), and requires **no** Windows Test-Signing.
* **Your Laptop gets 1–2 real virtual monitors (`IddCx 1.2`):** Windows sees your PC's monitors (e.g. `2560×1440 @ 170 Hz HDR` + `1920×1080 @ 75 Hz SDR`) as native extended displays — or mirrors your Laptop screen driver-free.
* **Featherweight Pure C++20:** No Electron, no .NET runtime, no WebView2 (~2–5 MB RAM, instant startup, zero GC micro-stutters).

---

## Screenshots

| PC Receiver — Overview & Live Telemetry | Laptop Sender — Connection & Live Quality |
| :---: | :---: |
| ![Receiver Overview](docs/images/receiver-overview.png) | ![Sender Connection](docs/images/sender-connection.png) |

| PC Receiver — Drag & Drop File/Folder Transfer | Laptop Sender — Power, Lid-Close & KVM Settings |
| :---: | :---: |
| ![Receiver Transfer](docs/images/receiver-transfer.png) | ![Sender Settings](docs/images/sender-settings.png) |

| Always-On In-Stream Quick-HUD (`FPS \| Latency \| Mbps \| KVM`) | PC Receiver — Settings & In-Stream HUD Toggle |
| :---: | :---: |
| ![In-Stream Quick-HUD](docs/images/stream-hud.png) | ![Receiver Settings](docs/images/receiver-settings.png) |

---

## Key Features

* **1 to 2 Virtual Displays (`IddCx 1.2`) + Live Mode Switching:** Extend onto all PC monitors, Monitor 1 only, Monitor 2 only, or Mirror Laptop screen — switchable on the fly while connected.
* **Triple-Codec Pipeline (Live-Switchable) + Dynamic Auto Bitrate:**
  * **H.264 & HEVC (H.265) Low-Latency (`AMF Direct` / `QuickSync` / `NVDEC Direct`):** Direct AMD `amfrt64.dll` ultra-low-latency encoder + Zero-Copy NV12 GPU shader decoding, 0 B-frames, 0-frame DPB reorder delay.
  * **Dynamic Auto Bitrate (`Auto (Dyn.)`) & Flexible Presets:** Automatically picks optimal bitrate bounds from monitor resolution, FPS, codec efficiency, and multi-monitor layout, and adapts live to Wi-Fi throughput, encoder load, and UDP packet loss — alongside manual `15–200 Mbps` and `Auto / 30 / 60 / 90 / 120 FPS` presets.
  * **Delta Lossless (`DLT3`, RGB 4:4:4):** GPU Compute (`cs_5_0` 64×64 tile diff) + pure-C++20 **QOI** lossless hybrid + XOR/LZ4 + DXGI `MoveRects` — **100% lossless and razor-sharp even while scrolling webpages** (zero blurry downscaling).
* **UDP Video + Reed-Solomon FEC & Seamless Auto-Reconnect:** Streams frames over UDP (`48216–48217`) with GF(2⁸) Cauchy RS-FEC parity (zero TCP head-of-line blocking) + automatic TCP fallback and seamless stream socket auto-reconnect without closing stream windows.
* **1-Click PC-Initiated Remote Connect (`Trusted PCs`):** Start a session directly from your Gaming PC — either from the Receiver Overview card (with live Laptop online status) or with 1 click from the PC System Tray. Only trusted PCs (`sender.ini` `[TrustedDevices]`) can wake a connection.
* **True `scRGB` HDR & Per-Monitor Color Calibration:** 16-bit floating-point `scRGB` output with `DISPLAYCONFIG_SDR_WHITE_LEVEL` matching + live Brightness / Gamma / Contrast / Saturation / RGB Gain sliders.
* **Driver-Free 1000 Hz Full-Desktop KVM:** Seamless mouse & keyboard control across all Laptop + virtual monitors using Raw Input (`WM_INPUT` + `ClipCursor`), independent **KVM Mouse & Scroll Speed** sliders (`25–500%`), and `Ctrl+Alt+K` toggle.
* **Always-On In-Stream Quick-HUD:** Compact D3D11 overlay at the top of the stream window showing live **FPS, Decode Latency (ms), Bitrate (Mbps), Codec, and KVM status**, with hover action buttons (`KVM`, `Fullscreen/Window`, `Hide`) and a 1-click toggle in Settings (`Ctrl+Alt+H`).
* **Bidirectional Clipboard & Recursive Folder/File Transfer:** Syncs text & images (`CF_DIB`), and sends files or entire nested folders via Drag & Drop with **overall batch percentage, transferred/remaining MB, speed, Cancel, and completion alert**.
* **Laptop Lid-Close Guard & Screen Dimming:** Optionally pauses Windows sleep on lid close (`PowrProf` + crash-safe INI restore) and blacks out physical Laptop screens (`ESC` / double-click to wake) while connected.
* **Windows x64 Installer, Battery-Safe Autostart & 1-Click Auto-Updater:** Bilingual Inno Setup installer (`Screen2PC-Setup-x64.exe`) with Laptop/PC role selection, automatic driver/firewall/autostart setup, portable ZIP option, and built-in GitHub Auto-Updater (`Auto / Deutsch / English`).

---

## Quickstart

### Option A: Windows Installer (Recommended)
1. **Download:** Grab `Screen2PC-Setup-x64.exe` from the [Latest Release](https://github.com/Aminomed/Screen2PC/releases/latest) on both your **PC** and **Laptop**.
2. **On your Gaming PC:** Run the installer and choose the **PC (Receiver)** role — stays **100% driver-free**, configures private-LAN firewall rules, and optionally enables tray autostart.
3. **On your Laptop:** Run the installer and choose the **Laptop (Sender)** role — automatically installs the `Screen2PC.Driver` UMDF v2 virtual display driver (**no Test-Signing required**), opens UDP `48218` for PC-initiated connect, and optionally sets up battery-safe autostart.
4. **First-Time Pairing:** Open **Screen2PC Sender** on your Laptop, select your PC, click **Connect (`Ctrl+Shift+K`)**, and enter the 6-digit PIN shown on your PC. Once paired, both devices trust each other — future connections work automatically from either the **Laptop** or with **1 click from your PC / PC Tray**.

### Option B: Portable ZIP
* Download `Screen2PC-Windows-x64.zip` from the [Latest Release](https://github.com/Aminomed/Screen2PC/releases/latest) and extract it on both devices.
* On your **Gaming PC**, run `Screen2PC.Receiver.exe` (zero install; optionally click **Open Firewall Ports (Admin)** in Settings).
* On your **Laptop**, run `scripts\install_driver.ps1` as Admin once (for extended virtual monitors), then start `Screen2PC.Sender.exe`.

> **Note on Windows SmartScreen:** Because Screen2PC is an independent open-source project without an expensive EV code-signing certificate, Windows SmartScreen may show *"Windows protected your PC"* on first launch. Click **More info → Run anyway**, or build directly from source using Visual Studio 2022 / GitHub Actions.

---

## Security & Privacy

* **100% Local Network Only:** Video, audio-free desktop frames, KVM input, clipboard, and files travel exclusively between your Laptop and PC on your LAN. The only optional outbound request is the GitHub Releases API check for updates (can be disabled in Settings).
* **Cryptographic 6-Digit PIN + Mutual Trust Lists:** Unknown senders are challenged with a `BCryptGenRandom` 6-digit PIN (`Pairing.cpp`, constant-time verification). An active session is never replaced until protocol version and PIN/MAC trust succeed. Remote connect triggers from the PC (UDP `48218`) are strictly verified against the Laptop's `TrustedDevices` list and can be disabled in Settings at any time.
* **Path-Traversal Protection:** File/folder transfers sanitize all relative paths (`SanitizeRelativePath`), block `..` / UNC / drive-letter escapes, and write only inside `%USERPROFILE%\Downloads\Screen2PC`.
* **Clean Uninstall:** Uninstall via Windows **Apps & Features** (when installed with `Screen2PC-Setup-x64.exe`) or run `scripts\uninstall_driver.ps1` (Admin) on your Laptop to remove the virtual display driver cleanly.

---

<details>
<summary><strong>Under the Hood — Architecture & Low-Latency Pipeline (Geeky Deep-Dive)</strong></summary>

### Stack & Footprint
* **Language:** Pure **C++20** (`stdcpp20`, `x64`, `/MT` static CRT, Visual Studio 2022 `v143`) — zero external runtime dependencies.
* **APIs:** Win32 (`Per-Monitor DPI v2`, `Common Controls v6`), D3D11 + Compute Shaders (`cs_5_0`), DXGI 1.6 (`IDXGIOutput6`, `ALLOW_TEARING`, Waitable SwapChain), Direct AMD AMF (`amfrt64.dll`), Media Foundation, Winsock2, `IPHelper`, `BCrypt`, `PowrProf`, WinHTTP, UMDF v2 (`IddCx 1.2`).

### Network Ports (`src/common/Protocol.h` — Protocol `v3`)
| Port | Protocol | Purpose |
| :--- | :--- | :--- |
| `48210` | UDP | LAN Discovery (`DiscoverReq` / `DiscoverResp` / `VersionMismatch`) |
| `48211` | TCP | Control Channel (Auth, `ReconfigureStream`, `ClipboardSync`, Reliable KVM fallback, Heartbeat) |
| `48212–48213` | TCP | Per-monitor video stream + cursor shape updates + large-frame (`> 256 KB`) fallback (`64 KB`/`128 KB` buffers) |
| `48214` | TCP | Dedicated recursive folder/file transfer (`64 KB` chunks, CRC32 verified) |
| `48215` | UDP | **Realtime KVM & Cursor Channel** (`THREAD_PRIORITY_HIGHEST`, `kMouseMove` UDP-only, buttons/wheel deduplicated dual UDP+TCP) |
| `48216–48217` | UDP | **Video + GF(2⁸) Cauchy RS-FEC Channel** (`1360 B` shards, recovers Wi-Fi packet loss without TCP head-of-line blocking) |
| `48218` | UDP | **PC-Initiated Remote Connect & Online Probe** (verified against Laptop `[TrustedDevices]` + `AllowRemoteConnect`) |

### Low-Latency Engineering
1. **Sub-Millisecond Mouse (`WM_INPUT` + UDP):** Raw HID mouse deltas flush immediately (`<= 1 ms` / 1000 Hz) over UDP port `48215` with sub-pixel speed accumulators (`KvmInput`).
2. **Direct AMD AMF (`amfrt64.dll`) + Dynamic Auto Bitrate:** Loads `amfrt64.dll` via pure C-ABI (`AMFVideoEncoderVCE_AVC` / `AMFVideoEncoderHW_HEVC`) with `ULTRA_LOW_LATENCY`, 0 B-frames, 3-slot `AllocSurface` ring, and real-time Auto-Bitrate scaling driven by network send time, encoder load, and UDP packet-loss feedback.
3. **Waitable `FLIP_DISCARD` + `ALLOW_TEARING`:** `D3D11Renderer` runs a `MsgWaitForMultipleObjectsEx` loop synchronized directly to `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (MaxFrameLatency `1`), decoupled from the `recvDecodeThread`.
4. **Zero-Copy DXVA `NV12` → `scRGB` Shader:** Decoded H.264/HEVC `NV12` surfaces are copied directly on the GPU (`CopySubresourceRegion`) and converted in `PSMainNv12` (BT.709 Studio → `scRGB` HDR or SDR) — skipping `VideoProcessorBlt`.
5. **GPU Compute Delta + `DLT3` QOI Hybrid:** A `cs_5_0` compute shader diffs `64×64` tiles on the GPU (zero pixel readback when idle), encodes large updates with pure-C++20 lossless QOI and small UI updates with XOR+LZ4 + `MoveRects`, and uploads only the dirty bounding box (`D3D11_BOX`) — keeping text 100% razor-sharp even while scrolling.
6. **In-Stream Quick-HUD:** Rendered as a cached premultiplied GDI-DIB → D3D11 texture quad inside `D3D11Renderer::Present()` so DWM stays in hardware Independent Flip (`ALLOW_TEARING`) with zero popup composition penalty.

</details>

---

## Building from Source

* **Requirements:** Visual Studio 2022 (`v143` C++20), Windows 11 SDK + WDK NuGet (`10.0.26100.2454` via `packages.config`).
* Open `Screen2PC.sln`, restore NuGet packages, and build **`Release | x64`**.
* Run `out\Release\x64\Screen2PC.Tests.exe` to execute the automated unit & integration test suite.

---

## License

Licensed under the [MIT License](LICENSE).
