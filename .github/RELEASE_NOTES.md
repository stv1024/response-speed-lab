**Feel every millisecond.** A native Windows tool that measures the real input-to-photon latency loop — from the hardware input timestamp to the vblank where pixels actually hit the screen.

## Download

| File | What it is |
|---|---|
| `ResponseSpeedLab-v1.0.0-x64.zip` | Executable + READMEs + license |
| `ResponseSpeedLab.exe` | Just the executable (~670KB) |
| `SHA256SUMS.txt` | Checksums |

Windows 10/11 x64. No installer, no runtime dependencies — statically linked, depends only on system DLLs. Double-click to run, `Esc` to quit.

## What it measures

Most latency demos time a `requestAnimationFrame` callback — which runs *before* paint and composition — and call it "time to screen." It isn't.

This measures the actual closed loop: **hardware input timestamp → message dispatch → UI logic → frame present → vblank scanout**, using `POINTER_INFO.PerformanceCount` on the input side and `DXGI_FRAME_STATISTICS::SyncQPCTime` on the output side.

## Measured on a 60Hz display, windowed

| Scope | min | avg | max |
|---|---:|---:|---:|
| To program (input → message) | 0.10 | 5.4 | 16.1 |
| To screen, **vsync-aligned** | **33.78** | 42.1 | 49.2 |
| To screen, **tearing enabled** | **17.76** | 35.4 | 49.2 |

Enabling tearing saves exactly one full frame. That's the real price of waiting for the next vblank — and it's the single most worthwhile thing to verify with your own hands.

## Features

- **Instant response** — button with zero animation; reports to-program / to-logic / to-screen separately, with min / avg / p95 / max
- **Latency samples** — `0 / 5 / 10 / 16 / 33 / 50 / 100 / 200 / 400 / 800 ms`; targets ≤20ms resolve within the same frame, so a 5ms sample doesn't silently cost an extra frame
- **Manual dial** — 0–500ms
- **Reaction time** — timed from the vblank where green actually appeared, so it excludes display latency
- **Reference tables** — perceptual milestones plus practical targets across Web / mobile / desktop / games / network, with measurement scopes kept explicitly separate

## Honest limits

No pure-software approach can measure mouse sampling and debounce (~1ms at 1000Hz, ~8ms at 125Hz), USB transport, or the display's internal overdrive / scaling / panel response (another 3–15ms). True ground truth needs NVIDIA LDAT or a high-speed camera. This is stated in the app's own measurement-scope panel.

The ~33ms floor in windowed mode is DWM desktop composition, not the app.

---

Built with [Dear ImGui](https://github.com/ocornut/imgui). MIT licensed. See [README](https://github.com/stv1024/response-speed-lab#readme) · [中文说明](https://github.com/stv1024/response-speed-lab/blob/main/README.zh-CN.md)
