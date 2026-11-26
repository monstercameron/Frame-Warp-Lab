# Development log

## v0.10.2 — Record five core architectural requirements in research docs

- Added the five core architectural pillars (deadline-based scheduling beyond VSync alone, independent scene-rendering and reprojection queues, GPU execution headroom reservation, actual displayed-frame timing through PresentMon, and independent camera vs. world-state update frequencies) to `research.md` and `experiment-plan.md`.
- Documentation-only change; verified with `git diff --check`. No code or binaries were modified.

## v0.10.1 — Record 10-step prioritized experiment roadmap

- Added the 10-step prioritized experiment sequence (from physical display timing instrumentation and independent render/warp scheduling through motion-vector extrapolation, temporal history, guard bands, independent frame generation, VRS/dynamic resolution, adaptive scheduling, resolution scaling benchmarks, and complex game-like workloads) to `research.md` and `experiment-plan.md`.
- Documentation-only change; verified with `git diff --check`. No code or binaries were modified.

## v0.10.0 — ARM64 NEON SIMD hot paths and HLSL shader optimization pass

- Added `src/simd_math.h` providing native ARM64 NEON (`<arm64_neon.h>`, reported as `simd_backend: "arm64_neon"` in CLI `capabilities`) with portable scalar fallback:
  - 128-bit NEON streaming stores (`vst1q_f32`, `vst1q_u32`, unrolled 4x for 64-byte write-combining lines) writing `SceneConstants`, `PresentConstants`, and `HudConstants` directly into persistently mapped D3D12 upload heaps without intermediate stack copies.
  - NEON-vectorized camera position advance + clamping (`vaddq_f32`, `vminq_f32`, `vmaxq_f32`) in `src/simulation.h` and NEON `float64x2_t` min/max/mean reductions in `src/cli.cpp`.
- Eliminated per-frame heap allocations and 2 KB struct copies by replacing `std::array<std::string, 4>` in `HudOverlayState` with fixed `char[4][64]` buffers passed by pointer, and skipped `reprojection_stats` UAV clear/copy barriers on non-warped frames.
- Optimized `shaders/motion_lab.hlsl` and `shaders/present.hlsl` with `D3DCOMPILE_OPTIMIZATION_LEVEL3`: added AABB culling around the 5-pillar cluster in the raymarch inner loop, replaced 6-tap central-difference normals with 4-tap tetrahedron gradients, tightened raymarch max distance to 36 m, and moved viewport reciprocals and horizontal FOV tangents to CPU constant buffers.
- Verified zero-warning ARM64 Release and Debug builds and live Release measurements on `NVIDIA RTX Spark N1X` (`1264x681`, `120 Hz` DWM deadline lock):
  - `scene_gpu_us` (`full_render`, `stress=0`) dropped from 660 µs (`v0.9.0`) to **154 µs** (~4.3x GPU speedup).
  - `present_gpu_us` (`full_render`) dropped from 55 µs to **28 µs** (~2x speedup).
  - `render_submit_cpu_us` (`reproject /2`) dropped from 259–276 µs to **179 µs** (min 72 µs); `sim_cpu_us` median 2 µs.
  - `reproject /2` (`occlusion_sweep`, `stress=0`, 90 frames) held 0 missed deadlines with `cpu_present_interval_us` median **8,333 µs**, p95 **8,446 µs**, max **8,587 µs**.
- Limitations: physical display scanout timing (`MsBetweenDisplayChange`) and elevated FrameView/PresentMon PCL capture remain to be run from an elevated desktop session; VRS Tier 2 rendering, async multi-queue scene/warp decoupling, and Streamline/DLSS-G integration remain to be built.

## v0.9.0 — DWM phase-locked high-resolution deadline frame pacer

- Added `src/frame_pacer.h` implementing a hybrid high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) + QPC spin-wait deadline pacer locked to `DwmGetCompositionTimingInfo` (`qpcRefreshPeriod` and `qpcVBlank` phase, linked via `dwmapi`), supporting `deadline` (default), `vsync_only`, and `unlocked` modes plus configurable `target_fps` (`0` = exact display refresh or `30..500`).
- Paced the `Present()` call entry (`MsBetweenPresents` semantics matching PresentMon/FrameView) after dispatching D3D12 command lists while keeping `wait_for_input_window()` before input sampling so Raw Input deltas are sampled just-in-time (~900 µs before `Present()`). Elevated the render thread to `THREAD_PRIORITY_HIGHEST`, bumped swap-chain latency limit to match the 2 back buffers (`max_frame_latency: 2`), and counted genuine deadline overruns in `missed_deadlines`.
- Updated the 2D HUD (`max_graph_ms = 20.0`, `PACE LOCK/VSYNC/FREE`, `MISS <n>`), hotkey `L` to cycle pacer modes, and CLI (`schema_version: 6`) with `set <pid> pacer deadline|vsync_only|unlocked` and `set <pid> target_fps <0|30..500>`.
- Verified both ARM64 Debug and Release builds with zero `/W4` warnings and live Release measurements on `NVIDIA RTX Spark N1X` (`1264x681`, `\\.\DISPLAY6` nominal 120 Hz):
  - `120 Hz` DWM deadline lock (`reproject /2`, `stress=48`, 90 frames): `cpu_present_interval_us` median **8,333 µs**, p95 **8,334 µs** (+1 µs spread), `sim_cpu_us` median 3 µs, `scene_gpu_us` median 3,689 µs, `present_gpu_us` median 43 µs (1 missed deadline during an 11.1 ms GPU spike, honestly recorded).
  - Fixed `144 Hz` deadline lock (`reproject /2`, `stress=0`, 90 frames): 0 missed deadlines, `cpu_present_interval_us` median **6,944 µs**, p95 **7,004 µs**.
- Limitations: `cpu_present_interval_us` measures CPU `Present()` entry cadence (`MsBetweenPresents`), not physical display scanout (`MsBetweenDisplayChange`), which still requires elevated PresentMon/FrameView ETW capture.

## v0.8.0 — Low-latency wait-before-input loop, Win32 Raw Input, and NVIDIA PCL ETW telemetry

- Reordered the render loop in `src/main.cpp` and `src/renderer.cpp` to split `Renderer::wait_for_frame_slot()` from `render_and_present()`, waiting on the waitable swap chain and GPU fence *before* pumping the latest input and constructing the camera pose.
- Added Win32 Raw Input (`RegisterRawInputDevices` for `HID_USAGE_GENERIC_MOUSE` + `WM_INPUT` `GetRawInputData`) so hardware mouse deltas bypass Windows cursor acceleration and `SetCursorPos` round-trip latency, with `WM_MOUSEMOVE` kept as fallback when Raw Input is absent.
- Added clean-room NVIDIA PC Latency (PCL) ETW instrumentation in `src/latency_monitor.h` and `src/latency_monitor.cpp` using Windows SDK TraceLogging (`PCLStatsTraceLoggingProvider`, GUID `0d216f06-82a6-4d49-bc4f-8f38ae56efab`, ping message `PC_Latency_Stats_Ping`), emitting `SIMULATION_START/END`, `CAMERA_CONSTRUCTED`, `RENDERSUBMIT_START/END`, `LATE_WARP_SUBMIT_START/END`, `PRESENT_START/END`, `LATE_WARP_PRESENT_START/END`, `PC_LATENCY_PING`, and `TRIGGER_FLASH`.
- Added an optional left-edge optical flash square (`flash` setting / `F` key) for NVIDIA Reflex Latency Analyzer (RLA) / LDAT hardware verification, in-process CPU latency stage timings (`input_to_present_us`, `sim_cpu_us`, `render_submit_cpu_us`, `present_call_cpu_us`), and CLI `capabilities` probing for local ARM64 NVIDIA latency tools.
- Verified ARM64 Debug build with zero `/W4` warnings and live CLI run on `NVIDIA RTX Spark N1X` (`1264x681`, `\\.\DISPLAY6` nominal 120 Hz):
  - Moving `wait_for_frame_slot()` before input sampling cut `cpu_source_age_us` median in `full_render` from 8,330 µs (`v0.7.0`) to 985 µs (`v0.8.0`), with `sim_cpu_us` median 11 µs, `render_submit_cpu_us` median 429 µs, and `present_call_cpu_us` median 422 µs.
  - `capabilities` detected native ARM64 `nvapia64.dll`, `FvSDK_ARM64.dll`, `PresentMon_arm64.exe`, and `nvrla.exe` installed on this machine; `status` reported `raw_input_enabled: true` and 232 PCL ETW markers emitted.
- Limitations: ETW capture by FrameView/PresentMon requires elevated privileges or Performance Log Users membership (`FvSvc` is currently stopped on this host, so `pcl_etw_listener_active` was `false` during the unprivileged test); CPU Present return interval under plain VSync still jitters across ~6.1–9.3 ms, which will be addressed next by precision deadline pacing.

## v0.7.0 — 2D HUD frametime graph, live FPS/frametime readout, and mouse-move camera controls

- Added a GPU-composited 2D HUD layer in `shaders/present.hlsl` (`HudConstants : register(b1)`) rendering a real-time 128-frame frametime graph (color-coded by `rendered` cyan, `repeated` amber, `warped` emerald, and `fallback` magenta, with nominal target and 16.67 ms reference lines plus GPU pass time overlay) and a 4-line 5x7 bitmap telemetry readout showing smoothed Present cadence FPS, frametime (ms), mode/divider, Scene/Warp GPU times (ms), source age (ms), invalid-hole percentage, camera path, stress, and VSync state. Toggleable cleanly with `O` or CLI `set <pid> overlay off`.
- Added interactive mouse-move camera controls in `src/main.cpp` and `src/simulation.h`: left-button drag look, right-click or `M` cursor-locked free mouse-look (`ESC` / focus loss safely unlocks), seamless handoff from any scripted path to `manual` preserving current pose, plus `WASD`/`Q`/`E` movement and interactive hotkeys (`1-3` mode, `H` holes, `O` HUD, `V` VSync, `C` cycle paths, `P`/`Space` pause, `R` reset, `[`/`]` divider, `-`/`=` stress).
- Extended `RuntimeState` (`runtime_state_version = 7`) and CLI (`schema_version: 4`) to report `smoothed_present_interval_us`, `smoothed_present_fps`, `camera.yaw_deg`, `camera.pitch_deg`, and `camera.mouse_look_active`, and updated `frame_warp_cli start` to request `SW_SHOWNORMAL`.
- Verified ARM64 Debug build with zero `/W4` warnings and live tests on `NVIDIA RTX Spark N1X` (`1264x681`, `\\.\DISPLAY6` nominal 120 Hz): `present_gpu_us` median was 52 µs with `overlay=on` vs 53 µs with `overlay=off` in `full_render`, and 129 µs with `overlay=on` in `reproject /4`. Posted left-mouse drag (`+180 px`, `-60 px`) cleanly transitioned to `manual` and rotated the camera to exact `yaw_deg: 33.00`, `pitch_deg: 11.00`.
- Limitations: HUD FPS and frametime reflect CPU Present return cadence (not physical display scanout); mouse-look and HUD rendering have been verified functionally via CLI/telemetry, not visually on the user's physical display due to the isolated command desktop.

## v0.6.0 — Repeat control, depth-based late camera reprojection, waitable tearing swap chain, and hole telemetry

- Added `repeat` and `reproject` presentation modes alongside `full_render`, controllable `source_divider` (1..8 display frames per full scene render), explicit frame-kind accounting (`rendered`, `repeated`, `warped`, `fallback`), safe camera-cut and stale-source fallbacks (`reprojection_fallback` events), and manual keyboard camera movement (`path=manual` using WASD + arrow keys).
- Implemented 4-iteration depth-guided backward camera reprojection in `shaders/present.hlsl` with round-trip consistency and 4-neighbor depth-edge checks, a magenta `holes` diagnostic view, and a GPU `RWByteAddressBuffer` invalid-pixel counter (`invalid_pixel_count` and `invalid_pixel_ratio`) resolved per swap-chain slot.
- Enabled `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (`SetMaximumFrameLatency(1)`) and `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when supported, plus runtime `vsync` (`on`/`off`) control.
- Expanded CLI (`schema_version: 3`) with `set <pid>` support for `mode`, `divider`, `vsync`, and `view=holes` / `path=manual`, plus `trace <pid> [limit] summary` for compact statistical summaries.
- Implementation surprises caught and resolved:
  - Shader Model 5.0 pixel shader compilation failed with FXC `error X4509` when binding the stats UAV to `register(u0)` because `SV_Target0` shares slot 0; moving the UAV to `register(u1)` resolved it cleanly.
  - Because double-buffered GPU readback lags by 2 frames, non-matching dividers (`divider=4`) initially attributed a completed warped slot's invalid-pixel count to a newly rendered frame's trace entry; adding explicit `gpu_slot_warped` tagging fixed summary attribution (45/45 warped samples at `divider=4`, 101/101 at `divider=8`).
  - A 2.0-second `camera_cut` interval synchronized with 120 Hz * 8 = 240 frames and always landed on rendered frames after reset; changing the cut period to 1.63 s reliably exercised `camera_cut` fallback on warped frames.
- Verified ARM64 Debug build with zero `/W4` warnings and comparative live CLI runs on `NVIDIA RTX Spark N1X` (`1264x681`, `\\.\DISPLAY6` nominal 120 Hz / 8333 µs interval):
  - At `stress=0`: `scene_gpu_us` median 630–695 µs; unwarped `present_gpu_us` median 31–52 µs; warped `present_gpu_us` median 118–124 µs (~0.12 ms). Invalid-pixel ratio rose from median 0.079% (`occlusion_sweep`, `divider=2`, max source age 17.3 ms) to median 1.95% / max 3.99% (`fast_pan`, `divider=4`, max source age 34.5 ms).
  - At `stress=48` (`occlusion_sweep`, VSync on): `scene_gpu_us` median ~3.80 ms. `full_render` spiked to max `cpu_present_interval_us` 16,230 µs (missed 120 Hz interval), whereas `repeat` (`divider=2`) and `reproject` (`divider=2`) held max `cpu_present_interval_us` at 9,613–9,775 µs while `reproject` added only ~43–65 µs GPU warp cost at boosted clocks.
  - With `vsync=off` (`allow_tearing_enabled=true`), `reproject` (`divider=2`, `stress=48`) reached median `cpu_present_interval_us` 3,903 µs (min 150 µs on warp-only frames).
  - With `camera_cut` (`divider=8`), 4 `reprojection_fallback` events (`camera_cut`) fired cleanly across the cut boundary.
- Limitations: `cpu_present_interval_us` is Present return cadence, not physical display scanout timing (PresentMon/FrameView and verified upper-bound monitor validation remain to be integrated); single-threaded submission means scene and warp still share one direct queue; motion-vector object prediction, VRS rendering, and NVIDIA Streamline/DLSS-G ARM64 verification remain to be built.

## v0.5.0 — CPU camera simulation, offscreen G-buffer targets, GPU timing, and trace CLI

- Split rendering into cohesive modules (`src/simulation.h`, `src/renderer.h`, `src/renderer.cpp`) and moved camera basis, deterministic Motion Lab paths (`corridor_sway`, `fast_pan`, `occlusion_sweep`, `camera_cut`), moving-ball kinematics, and discontinuity flags to the CPU.
- Updated `shaders/motion_lab.hlsl` to render into three offscreen targets (`R16G16B16A16_FLOAT` linear color, `R32_FLOAT` linear view-space depth, `R16G16_FLOAT` analytical 2D screen-space motion vectors) with a controllable synthetic shading stress loop (`stress_level` 0..64). Added `shaders/present.hlsl` to tone-map or visualize depth/motion buffers and composite a toggleable late center reticle.
- Added D3D12 timestamp queries (`D3D12_QUERY_HEAP_TYPE_TIMESTAMP` + readback buffer) measuring non-stalling per-slot GPU durations (`scene_gpu_us` and `present_gpu_us`) alongside a bounded 256-sample frame trace ring in shared memory (`runtime_state_version = 5`).
- Added CLI `set <pid> <key> <value>` (`view`, `path`, `stress`, `overlay`) and `trace <pid> [limit]` with min/median/p95/max summaries; bumped CLI JSON `schema_version` to 2.
- Verified ARM64 Debug build with zero `/W4` warnings and live CLI run on the RTX Spark N1X (`1264x681`, `\\.\DISPLAY6` nominal 120 Hz): at `stress=0`, median `scene_gpu_us` was 661 µs and `present_gpu_us` was 29 µs (`cpu_present_interval_us` median 8401 µs); at `stress=12`, median `scene_gpu_us` rose to 1201 µs while `present_gpu_us` remained 29 µs. These measurements cover GPU pass cost and CPU Present cadence only, not physical display scanout timing or image quality.
- Repeat control mode, depth-based camera reprojection, tearing/waitable swap chain control, manual camera input, VRS rendering, and NVIDIA DLSS-G investigation remain to be built.

## v0.4.1 — Harden shared-state ordering, GPU shutdown, and capability reporting

- Replaced raw `volatile` IPC accesses with C++20 `std::atomic_ref` acquire/release operations and explicit acquire fences around event-journal reads so lock-free snapshots are ordered safely on Windows ARM64. Widened `scene_time_ms` to 64-bit integer milliseconds and bumped `runtime_state_version` to 4.
- Added a `noexcept`-guarded GPU `flush()` in `~Renderer()` so GPU work drains during exception unwinding, guarded zero-size client viewports against division-by-zero, passed explicit Win32 operation names into `Handle`, and logged `scene_paused`, `scene_resumed`, and `scene_reset` events into the bounded runtime journal.
- Deduplicated shared-memory mapping in `frame_warp_cli` behind an RAII view, JSON-escaped CLI error messages, widened the launch path buffer to 32768 wide characters, and expanded `capabilities` to report default adapter identity (`NVIDIA RTX Spark N1X`, 98290 MiB VRAM observed locally) and DXGI tearing support (`allow_tearing_supported: true`).
- Verified an ARM64 Debug build with zero `/W4` warnings and a live CLI pass (`capabilities`, `start`, `status`, `pause`, `reset`, `resume`, `events`, `stop`, `instances`). Observed nominal active display mode `2880x1800@120Hz` on `\\.\DISPLAY6` (unverified upper bound). Displayed-frame timing, offscreen G-buffers, reprojection, and frame generation remain to be built.

## v0.4.0 — Initial Motion Lab scene

- Added a procedural D3D12/HLSL scene with a moving camera, checkerboard corridor, alternating foreground pillars, moving patterned ball, fine rail and gridded end wall. Added CLI pause, resume and reset controls and scene-time inspection.
- The first runtime shader compilation failed because `point` and then `line` are reserved HLSL tokens. Renaming them and compiling the pixel shader with the Windows SDK shader compiler exposed and resolved the errors. Added a build dependency so shader edits are copied to the executable directory.
- Verified an ARM64 Debug build and a live run: status reported scene time, pause held the timeline, reset returned it to zero while paused, resume advanced it, and the CLI shut down the process cleanly. This checks process and control behavior; it does not establish display pacing, input latency or image quality.
- This is an initial procedural scene, not the complete planned Motion Lab. User-controlled movement, explicit scene geometry/depth and motion-vector buffers, telemetry captures, frame generation, reprojection, VRS rendering, and actual displayed-frame measurement remain to be built.

## Earlier project history

- v0.1.x established the research plans, prioritized combined frame generation and reprojection at the monitor's upper refresh bound, and added VRS as a research technique.
- v0.2.0 added the runtime inspection CLI foundation; v0.3.0 added launch control and a bounded in-memory event journal. The existing history predates the per-commit log requirement.
