<div align="center">

# Response Speed Lab

**Feel every millisecond.** A native Windows tool that measures the *real* input-to-photon latency loop — from the hardware input timestamp to the vblank where pixels actually hit the screen.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue)

English · [简体中文](README.zh-CN.md)

</div>

---

## Why this exists

Most "latency demo" pages measure the wrong thing. They time a `requestAnimationFrame` callback — which runs *before* paint and composition — and call it "time to screen." It isn't.

This tool measures the actual closed loop:

**hardware input timestamp → message dispatch → UI logic → frame present → vblank scanout**

| | Typical web version | This tool |
|---|---|---|
| **Measurement endpoint** | `requestAnimationFrame` callback — runs *before* paint/composite, systematically too small | `DXGI_FRAME_STATISTICS::SyncQPCTime` — the real vblank that carried the change |
| **Delay-sample precision** | `setTimeout(5)` actually fires in 5–20ms; the `5 / 10 / 16ms` samples are **indistinguishable** | High-resolution timer + QPC spin; short targets resolve within the same frame |
| **Clock precision** | `performance.now()` quantized to ~100µs without cross-origin isolation (coarser in Firefox) | QPC, ~0.1µs resolution |

## What it does

1. **Instant response** — A big button that changes color the moment it's pressed. Zero animation, zero easing. Reports three distinct scopes:
   - **To program** — input → message received
   - **To logic** — input → UI claims the press
   - **To screen** — input → vblank scanout

   With min / avg / p95 / max over the last 100 presses.

2. **Latency samples** — Ten buttons: `0 / 5 / 10 / 16 / 33 / 50 / 100 / 200 / 400 / 800 ms`. Compare them side by side. Targets ≤20ms are honored *within the same frame* so a 5ms sample doesn't silently cost an extra 16.7ms.

3. **Manual dial** — 0–500ms slider for any specific value.

4. **Reaction time** — Your human reaction time, timed from the vblank where green *actually appeared*, so the number excludes display latency.

5. **Reference tables** — Perceptual milestones plus practical target ranges across Web / mobile / desktop / games / network, with measurement scopes kept explicitly separate ("input to pixel" vs "network RTT" vs "action to content ready" are not interchangeable).

## Measured results

60Hz, windowed mode, 15+ samples each. Reproduced across independent clean builds:

| Scope | min | avg | max |
|---|---:|---:|---:|
| To program (input → message) | 0.10 | 5.4 | 16.1 |
| To screen, **vsync-aligned** | **33.78** | 42.1 | 49.2 |
| To screen, **tearing enabled** | **17.76** | 35.4 | 49.2 |

**Enabling tearing saves exactly one full frame (33.78 → 17.76ms).** That's the real price of "waiting for the next vblank" — and it's the single most worthwhile thing to verify with your own hands.

The ~33ms (two-frame) floor in windowed mode comes from DWM desktop composition. Borderless fullscreen hitting independent flip can go lower.

## Tech stack

**C++17 · Win32 · Direct3D 11 / DXGI flip model · Dear ImGui** — statically linked, single ~670KB exe, no runtime dependencies.

- **Input side** — `EnableMouseInPointer` → `WM_POINTERDOWN` → `POINTER_INFO.PerformanceCount`, the QPC timestamp stamped by the input stack when the hardware event arrived, earlier than message delivery.
- **Output side** — `FLIP_DISCARD` swapchain + `SetMaximumFrameLatency(1)` + waitable object; `GetFrameStatistics()` reads back when the frame actually scanned out.
- **Tearing toggle** — `ALLOW_TEARING` + `Present(0)`, scan out immediately without waiting for vsync.

<details>
<summary><b>Why not a web stack, C#/WPF, wgpu, or Electron?</b></summary>

- **Browser** — Cannot reach `SyncQPCTime`. `rAF` fires before composition, `setTimeout` can't resolve single-digit milliseconds, and the clock is deliberately coarsened. Also adds a browser-process → renderer-process IPC hop and can't bypass DWM.
- **C# / WPF / WinUI** — GC pauses. A latency measurement tool with its own uncontrollable stop-the-world pauses has no credibility. NativeAOT with zero allocation could work, but the constraints cost more than writing C++.
- **Rust + wgpu** — Rust itself is fine, but wgpu abstracts away `GetFrameStatistics`, tearing control, and waitable swapchains — exactly the things needed here. Rust with `windows-rs` calling DXGI directly would be equivalent.
- **Electron / Tauri** — Inherits every browser limitation, plus another layer.
- **Qt / WinUI 3** — Their own render loops and input queues aren't controllable, which contaminates the measurement scope.

</details>

## Build and run

**Build:** requires Visual Studio 2026 (18) with the C++ workload and Windows SDK.

```bat
build.bat
```

Or manually:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
```

Dear ImGui v1.92.1 is vendored in `third_party/imgui` — no dependency fetching required.

The build produces `bin\ResponseSpeedLab.exe`. Double-click to run; `Esc` to quit.

## Measurement scope and known limits

**What no pure-software approach can measure:**

- Mouse sampling and debounce — ~1ms at 1000Hz, ~8ms at 125Hz
- USB transport
- The display's internal overdrive / scaling / panel response — typically another 3–15ms

True ground truth (actual photons) requires NVIDIA LDAT or a high-speed camera. This is stated in the app's own "measurement scope" panel so `0.10 ms` is never mistaken for end-to-end.

**Other notes:**

- With tearing enabled, some display paths stop reporting DXGI frame statistics. The app then shows **"unmeasurable"** rather than a stale value.
- Cross-validate with [Intel PresentMon 2.x](https://game.intel.com/us/stories/intel-presentmon/) — its Click-to-Photon metric uses a comparable scope.
- The app sets `timeBeginPeriod(1)` and `HIGH_PRIORITY_CLASS` to reduce scheduler jitter contaminating the measurement.

## Project layout

```
response-speed-lab/
├── src/
│   ├── main.cpp        # Win32 window, frame loop, message pump
│   ├── renderer.cpp    # D3D11 + DXGI flip chain, frame statistics readback
│   ├── app.cpp         # Panels, probe lifecycle, statistics
│   ├── timing.h        # QPC primitives, high-resolution waiter
│   └── fonts.h         # Fonts and theme
├── third_party/imgui/  # Dear ImGui v1.92.1
├── bin/                # Built executable
├── build.bat
└── legacy-web/         # The original single-file HTML version (archived)
```

## License

[MIT](LICENSE) © stv1024

Dear ImGui is © Omar Cornut, also MIT licensed.
