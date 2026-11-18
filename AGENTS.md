# AGENTS.md — Screen2PC Engineering Guide

This file is the authoritative instruction & architecture reference for AI coding agents working in `Aminomed/Screen2PC`.

---

## 1. Project Overview & Target Hardware
- **Goal:** Ultra-low-latency local-LAN virtual display, streaming, KVM, clipboard, and folder/file-transfer suite written in pure **C++20** (`stdcpp20`, `x64`-only, `/MT` Release / `/MTd` Debug, VS 2022 `v143`) — zero third-party runtimes.
- **Sender (Surface):** Windows 11 x64, AMD Ryzen 5 Microsoft Surface Edition (AMD Radeon iGPU, 8 GB RAM, Wi-Fi). Runs `Screen2PC.Sender.exe` + UMDF v2 `IddCx 1.2` virtual display driver (`Screen2PC.Driver.dll`).
- **Receiver (Gaming PC):** Windows 11 x64, Nvidia RTX 4070, 1 Gbit/s LAN, Monitor 1 `2560x1440` HDR, Monitor 2 `1920x1080` SDR. Runs `Screen2PC.Receiver.exe`.

---

## 2. Hard Invariants (Never Violate)
1. **Receiver (Gaming PC) MUST remain 100 % driver-free and hook-free:**
   - Never install kernel/UMDF drivers, test-signing, or global `SetWindowsHookEx` hooks on the PC (`Screen2PC.Receiver.exe`).
   - All KVM input capture on the PC uses window-focused `WM_INPUT` (Raw Input) + `ClipCursor` + `SetCursor(nullptr)`.
2. **Authentication & Session Safety (`Protocol v2`):**
   - `ReceiverEngine` accepts only 1 active session at a time, and MUST NOT tear down an active prior session until protocol version (`kProtocolVersion = 2`) and PIN/MAC trust (`Pairing.*`) succeed.
3. **Power Policy Safety (`PowerPolicy.*`):**
   - `LidCloseGuard` may only set lid-close action to *Do Nothing* (`0`) + `ES_SYSTEM_REQUIRED` while `st.connected && st.usingExternalDisplays`. Always persist original AC/DC values atomically in `sender.ini` (`[PowerRestore]`) before modifying them, and restore on disconnect/exit/startup recovery.
4. **CI & Verification:**
   - Local Linux VM cannot compile Windows MSBuild/WDK targets directly. Every code change MUST be pushed to `main` and verified via `.github/workflows/ci.yml` (`Build Solution (Release|x64)`, `Screen2PC.Tests.exe`, `infverif.exe`, `Inf2Cat.exe`, Release bundle).
   - Documentation-only commits (`MEMORY.md`, `AGENTS.md`, `README.md`) should use `[skip ci]` in the commit message when no code changed.

---

## 3. Architecture & Ultra-Low-Latency Pipeline (`v1.5.0.33`)

### 3.1 Network Ports (`src/common/Protocol.h`)
| Port | Protocol | Purpose |
|---|---|---|
| `48210` | UDP | LAN Discovery (`DiscoverReq` / `DiscoverResp` / `VersionMismatch`) |
| `48211` | TCP | Control channel (Handshake, PIN/MAC auth, `StartSession`, `ReconfigureStream`, `KvmKey`, `KvmMouse` button/wheel reliable fallback, `ClipboardSync`, `Heartbeat`) |
| `48212–48213` | TCP | Per-monitor video stream + cursor shape updates (asymmetric buffers: `(256 KB, 64 KB)` Sender / `(64 KB, 512 KB)` Receiver) |
| `48214` | TCP | Dedicated recursive folder/file transfer (`FileTransfer.*`) |
| `48215` | UDP | **Realtime KVM & Cursor channel** (`kPortRealtimeUdp`): `KvmMouse` (`kMouseMove` UDP-only, buttons/wheel dual UDP+TCP deduplicated via `hdr.reserved` sequence) and `CursorUpdate` (`!shapeChanged`), running on `THREAD_PRIORITY_HIGHEST` |

### 3.2 Mouse & Cursor Low-Latency (`M1–M3`)
- **`D3D11Renderer` (`src/receiver/D3D11Renderer.cpp`):**
  - `WM_INPUT` flushes relative mouse deltas immediately (`FlushRelativeMouseMove()`, `<= 1 ms` / 1000 Hz) instead of batching on a timer.
  - `DXGI_SWAP_EFFECT_FLIP_DISCARD` + `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `FRAME_LATENCY_WAITABLE_OBJECT` (MaxFrameLatency `1`, `2 ms` wait timeout).
- **`KvmInput` (`src/common/KvmInput.cpp`):**
  - Caches monitor rects and `SM_XVIRTUALSCREEN` / `SM_YVIRTUALSCREEN` / `SM_CXVIRTUALSCREEN` / `SM_CYVIRTUALSCREEN` for 500 ms.
  - Only resyncs virtual cursor position from `GetCursorPos` on first use or after `> 250 ms` of inactivity (eliminates cursor micro-rubberbanding).

### 3.3 H.264 Ultra-Low-Latency Pipeline (`H1–H3`)
- **Encoder (`src/common/CodecH264.cpp`, `SenderEngine.cpp`):**
  - Constrained Baseline (`eAVEncH264VProfile_Base`, `0` B-frames, `CABAC=false`, `LowDelayVBR`, `CODECAPI_AVLowLatencyMode`, 2-frame tight VBV) + `EnsureLowLatencyH264Stream()` SPS patching (`constraint_set0_flag | constraint_set1_flag`) for 0-frame DPB reorder delay.
  - Detects AMD AMF runtime (`amfrt64.dll` / `AMFQueryVersion` -> `IsAmfActive()`, labeled `H.264 (AMF ULL)`).
  - Handles async hardware MFT startup warmup via `WasLastCallWarmup()` so initial `MF_E_TRANSFORM_NEED_MORE_INPUT` frames do not trigger false GPU->CPU fallbacks.
  - 4-slot NV12 GPU conversion ring + **decoupled 1-slot Latest-Frame-Wins async TCP sender thread** in `SenderEngine::RunStreamWorker` (replaces stale queued frames and forces next H.264 IDR if an H.264 delta was dropped).
- **Decoder & Direct NV12 (`src/common/CodecH264.cpp`, `D3D11Renderer.cpp`, `ReceiverEngine.cpp`):**
  - `H264Decoder` sets `CODECAPI_AVLowLatencyMode`, `CODECAPI_AVDecVideoFastDecodeMode = 32`, caches up to 24 `ID3D11VideoProcessorInputView` slots, and guards startup DPB warmup via `WasLastCallNeedMoreInput()`.
  - **Direct NV12 Shader Path (`HasNv12Pipeline()`):** `H264Decoder::Decode` copies DXVA NV12 slices directly into `D3D11Renderer::Nv12Texture()` (`CopyNv12TexToTarget`), skipping `VideoProcessorBlt`; `PSMainNv12` converts BT.709 Studio NV12 directly to `scRGB` HDR or SDR with full color calibration (`H.264 (NVDEC Direct)`).

### 3.4 Delta Lossless v2 (`DLT2`) & Lossless Settle (`D1–D2`)
- **`CodecDelta` (`src/common/CodecDelta.h/.cpp`):**
  - `DLT2` format (backward-compatible with `DLT1`): XOR-against-previous-frame + fast LZ4 block compression, true DXGI `MoveRects` (`DeltaMoveRect`, 0 pixel bytes), `MergeAndBoundRects` (coalesces arbitrary dirty rects up to 32 without discarding), and `LastBoundingRect()` for partial GPU upload.
  - **2×2 Fast-Motion Subsampling** when `allowFastMotion && dirtyArea >= 35%` (keeps encoder/decoder reference frames bit-exact).
- **Partial Readback/Upload & Lossless Settle:**
  - `DxgiCapture::CopyFrameToCpu` reads back only the bounding box of dirty/move rects via `CopySubresourceRegion`; `D3D11Renderer::QueueCpuFrame` / `UploadCpuBgra` uploads only `LastBoundingRect()` via `D3D11_BOX`.
  - After `120 ms` (Delta) or `150 ms` (H.264) of stillness, `SenderEngine` sends a 100 % lossless settle frame for pixel-perfect RGB 4:4:4 text.

---

## 4. Repository Map
| Path | Purpose |
|---|---|
| `AGENTS.md` | Agent engineering rules, invariants, architecture & repo map |
| `MEMORY.md` | Detailed project state, INI/registry specs, and chronological changelog |
| `README.md` | User-facing quickstart & feature documentation |
| `Screen2PC.sln`, `Directory.Build.props`, `packages.config` | VS 2022 Solution (`x64`, `SCREEN2PC_BUILD_NUMBER`), NuGet WDK/SDK `10.0.26100.2454` |
| `src/common/` (`Screen2PC.Common.vcxproj`) | `Protocol.h`, `Version.h`, `Resource.h`, `AppIcon.h`, `BitratePresets.h`, `Autostart.*`, `ColorProfile.*`, `Pairing.*`, `Updater.*`, `Logger.*`, `NetSocket.*`, `CodecH264.*`, `CodecDelta.*`, `ClipboardSync.*`, `FileTransfer.*`, `KvmInput.*` |
| `src/driver/` (`Screen2PC.Driver.vcxproj`) | UMDF v2 `IddCx 1.2` virtual display driver (`DriverProtocol.h`, `Driver.h/.cpp`, `Screen2PCDriver.inf`) |
| `src/driversetup/` (`Screen2PC.DriverSetup.vcxproj`) | Driver installer helper (`Screen2PC.DriverSetup.exe`) |
| `src/sender/` (`Screen2PC.Sender.vcxproj`) | Surface Sender app (`SenderEngine.*`, `DxgiCapture.*`, `PowerPolicy.*`, `VirtualDisplayCtl.*`, `main.cpp`) |
| `src/receiver/` (`Screen2PC.Receiver.vcxproj`) | PC Receiver app (`ReceiverEngine.*`, `D3D11Renderer.*`, `main.cpp`) |
| `src/tests/` (`Screen2PC.Tests.vcxproj`) | Automated unit & integration test suite (`Screen2PC.Tests.exe`) |
| `scripts/` | `install_driver.ps1`, `uninstall_driver.ps1`, `generate_icons.py` |
| `.github/workflows/ci.yml` | Windows-2022 CI: MSBuild `Release\|x64`, `Screen2PC.Tests.exe`, `infverif`, `Inf2Cat`, Artifact & GitHub Release |

---

## 5. Key Hotkeys, Config & Deferred Backlog
- **Hotkeys:** `Ctrl+Shift+K` (global KVM toggle on Sender), `Ctrl+Alt+K` (global KVM toggle in Stream window), `F11` (fullscreen toggle), `ESC` (exit fullscreen & release KVM capture), `X` / `Alt+F4` (hide-to-tray on main windows; hide-without-disconnect on Stream windows).
- **Config INI paths:** `%APPDATA%\Screen2PC\sender.ini` and `%APPDATA%\Screen2PC\receiver.ini`.
- **Deferred Feature Backlog (do not implement unless requested):**
  1. HEVC (H.265) / 10-Bit HDR end-to-end streaming
  2. WASAPI Loopback audio streaming (Surface -> PC)
  3. Seamless Edge KVM (automatic cursor transition across monitor edges)
  4. In-Stream Quick-HUD overlay
  5. Desktop Profiles (saved resolution/codec/KVM/power presets)
