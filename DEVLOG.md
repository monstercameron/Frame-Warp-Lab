# Development log

## v0.16.0 — Offline DXC Shader Model 6.0, wave-level atomics, precomputed camera homography, and specialized presentation PSOs

- Migrated shader compilation in `CMakeLists.txt` and `src/renderer.cpp` from runtime FXC (`vs_5_0`/`ps_5_0`, `d3dcompiler`) to offline build-time Windows SDK ARM64 `dxc.exe` compilation (`vs_6_0`/`ps_6_0`, `-O3 -WX`), loading precompiled DXIL `.cso` bytecode directly at startup.
- Added precomputed CPU-to-GPU relative projective homography columns (`homography_col_x/y/z`) in `PresentConstants` and split `shaders/present.hlsl` into 5 specialized PSOs selected per frame on the CPU based on mode, diagnostic view, and camera translation displacement:
  - `present_simple_ps.cso`: plain passthrough + overlays (`full_render` / `repeat`).
  - `present_rot_warp_ps.cso`: **0-depth-sample** projective homography for pure camera rotation (`translation < 0.5 mm`, e.g., mouse-look or the new deterministic `pure_rotation` camera path).
  - `present_depth_fast_ps.cso`: 2-iteration lightweight depth warp without 4-neighbor gradient checks for small translations (`0.5 mm <= translation < 4 cm`).
  - `present_depth_full_ps.cso`: 4-iteration full depth-guided warp with 4-neighbor edge checks (`translation >= 4 cm`).
  - `present_synthesis_ps.cso`: `frame_gen`, `combined`, and diagnostic views (`depth`, `motion`, `holes`, `vrs`).
- Replaced per-pixel global `reprojection_stats.InterlockedAdd` contention with uniform-control-flow Shader Model 6.0 wave reductions (`WaveActiveCountBits` + `WaveIsFirstLane`), cutting atomic traffic by up to 32x per wave.
- Updated `AGENTS.md` to require `git push` after every commit, and added the Shading and Resolution Technology Priorities table to `research.md` and `experiment-plan.md`.
- Verified zero-warning ARM64 Release and Debug builds (all 8 DXIL shaders compiled with `-WX`), Debug D3D12 Debug Layer smoke test on `rotation_homography`, and live 100-frame Release measurements on `NVIDIA RTX Spark N1X` (`1264x681`, `120 Hz` DWM deadline lock, `diagnostic` instrumentation, 0 / 500 missed deadlines across all 5 variants):
  - `simple` (`full_render`): `present_gpu_us` median **52 µs** (min 36 µs).
  - `rotation_homography` (`pure_rotation /4`, 0 depth samples): `present_gpu_us` median **53 µs** (min 41 µs) — within 1 µs of plain passthrough and **2.85x faster** than the previous 151 µs 9-sample depth solver, even while counting 2.2% out-of-frustum invalid pixels via wave reductions.
  - `depth_fast` / `depth_full` (`occlusion_sweep /4`): `present_gpu_us` median dropped from **151 µs (`v0.12.0` FXC SM 5.0)** to **104 µs** (**31% faster**).
  - `synthesis` (`combined`, `occlusion_sweep /4`): `present_gpu_us` median dropped from **232 µs** to **209 µs** (**10% faster**).
- Note on retained tradeoffs: `R32_FLOAT` linear depth was intentionally kept over `R16_FLOAT` to preserve depth-reprojection accuracy at corridor distances (~25 m), and the SDF raymarcher was retained as a controlled synthetic pixel workload for historical continuity.

## v0.15.0 — Wait/fence error hardening, seqlock IPC, exact GPU attribution, typed events, and instrumentation levels

- Hardened synchronization and failure handling in `src/renderer.h` and `src/renderer.cpp`:
  - `wait_for_frame_slot()` now explicitly inspects `WaitForSingleObjectEx` results (`WAIT_OBJECT_0`, `WAIT_IO_COMPLETION` retry, `WAIT_TIMEOUT` with `GetDeviceRemovedReason()` check returning `false` instead of presenting an unacquired slot, and throwing on `WAIT_ABANDONED` / `WAIT_FAILED`).
  - `wait_for_fence()` replaces `INFINITE` waits with bounded 5,000 ms wait intervals and `GetDeviceRemovedReason()` checks so TDR/device-loss events surface cleanly instead of hanging indefinitely.
  - Tagged each swap-chain buffer slot with its original `display_frame_id` and `source_frame_id`, reporting exact `gpu_measured_display_frame_id` and `gpu_measured_source_frame_id` in `RuntimeFrameSample` and CLI `trace`.
- Upgraded shared-memory IPC in `src/runtime_state.h` (`runtime_state_version = 14`), `src/main.cpp`, and `src/cli.cpp` (`schema_version: 10`) to a formal odd/even generation seqlock (`seqlock_write` / `seqlock_try_read`) and replaced magic-number config event payloads with typed `ConfigKey` fields (`"key":"mode"`, `"key":"divider"`, `"key":"instrumentation"`, etc.).
- Moved reprojection consistency thresholds (`1.75 px`, `4.5%` depth, `18%` edge jump, `0.65 px` edge threshold) into `PresentConstants::reprojection_thresholds`, replaced the 65% unwarped-image ghosting blend on invalid pixels with a conservative background-depth neighbor fill in `shaders/present.hlsl`, throttled HUD text formatting to 15 Hz (plus immediate refresh on config changes), and added configurable `InstrumentationLevel` (`minimal`, `standard`, `diagnostic`; CLI `set <pid> instrumentation ...`) gating ETW markers, HUD rendering, and UAV stats barriers/copies.
- Verified zero-warning ARM64 Release and Debug builds and live Release measurements on `NVIDIA RTX Spark N1X` (`occlusion_sweep`, `reproject /4`, `120 Hz`):
  - `trace` samples accurately report the 2-frame readback lag (`display_frame_id: 50` carrying `gpu_measured_display_frame_id: 48`, `gpu_measured_source_frame_id: 12`).
  - Switching `instrumentation` from `diagnostic` to `minimal` cut `render_submit_cpu_us` median from **145 µs → 117 µs**, `present_call_cpu_us` median from **265 µs → 231 µs**, and `sim_cpu_us` median from **5 µs → 3 µs** with 0 missed deadlines (`8,333 µs` median / `8,333 µs` p95).

## v0.14.0 — Adaptive deadline-aware scheduler with GPU execution headroom reservation

- Added `src/adaptive_scheduler.h` and wired it into `src/runtime_state.h` (`runtime_state_version = 13`), `src/main.cpp`, and `src/cli.cpp` (`schema_version: 9`):
  - Computes dynamic GPU execution headroom reservation (`max(25% of target_period_us, 2.5 * smoothed_present_gpu_us + 800 µs)`) and tracks smoothed `gpu_budget_utilization` against the active deadline and source divider.
  - Implements a 5-tier hysteresis-protected escalation ladder (`Tier 0`: `full_render /1, vrs=off` → `Tier 1`: `full_render /1, vrs=foveated` → `Tier 2`: `combined /2, vrs=foveated` → `Tier 3`: `combined /3, vrs=uniform_2x2` → `Tier 4`: `combined /4, vrs=uniform_2x2`) that preserves full-rate rendering first, activates Tier 2 foveated VRS second, and engages `combined` synthesis + cadence division under deeper GPU saturation.
  - Uses VRS gain compensation (`expected_vrs_cost_scale`) and a time-normalized recovery hold (`max(72 frames, 750 ms)`) plus a 24-frame post-transition settle window so stepping down does not oscillate, and gates miss-driven escalation by `gpu_budget_utilization >= 0.45` so isolated CPU/OS timer jitter does not degrade rendering quality when the GPU is idle.
  - Added hotkey `K` and CLI `set <pid> adaptive on|off` (manual mode/divider/VRS edits cleanly hand back manual control), `RuntimeEventType::scheduler_decision` journal logging, live `ADP T<n>` HUD telemetry, and `adaptive_scheduler` reporting in `status` and `trace`.
- Verified zero-warning ARM64 Release and Debug builds and live Release stress-ramp experiments on `NVIDIA RTX Spark N1X` (`1264x681`, `occlusion_sweep`, `deadline` pacer):
  - `120 Hz` (`8,333 µs` deadline): fixed synchronous baseline (`full_render`, `vrs=off`, `async_gpu=off`, `stress=64`) suffered **100 / 100 missed deadlines** (`cpu_present_interval_us` p95 10,882 µs, max 11,851 µs); with `adaptive=on` (`async_gpu=on`), RTX Spark N1X ran `stress=64` at `scene_gpu_us` median 4,479 µs (`72.8%` of the 6,250 µs budget after 2,083 µs reserved headroom) with **0 / 100 missed deadlines** and median **8,333 µs**.
  - `240 Hz` (`4,166 µs` deadline, `stress` `0 → 64 → 0` stress ramp):
    - Stage 1 (`stress=0`): held **Tier 0** (`full_render /1, vrs=off`, `decision_count: 0`, `240.10 FPS`).
    - Stage 2 (`stress=64`): escalated in **26 ms** via a single `scheduler_decision` to **Tier 1** (`foveated VRS`, dropping `scene_gpu_us` to **1,565 µs** median), holding `cpu_present_interval_us` median **4,166 µs**, p95 **4,167 µs** (+1 µs spread), 1 / 120 missed deadline, and zero oscillation.
    - Stage 3 (`stress=0` recovery): stepped cleanly back to **Tier 0** (`headroom_ok`) after the hysteresis hold with **2 total scheduler decisions** across the full ramp.
- Honest limitations: the fixed 240 Hz test paces CPU `Present()` entry at 240 Hz on a nominally 120 Hz display (testing scheduler overload behavior, not 240 Hz physical display scanout); VRS cost scaling factors are calibrated for pixel-bound Motion Lab workloads and may need per-workload tuning on geometry- or memory-bound scenes.

## v0.13.0 — Direct3D 12 Variable Rate Shading (Tier 1 per-draw and Tier 2 foveated image)

- Added hardware Variable Rate Shading support in `src/renderer.h`, `src/renderer.cpp`, `shaders/present.hlsl`, `src/runtime_state.h` (`runtime_state_version = 12`), `src/main.cpp`, and `src/cli.cpp` (`schema_version: 8`):
  - Queries `D3D12_FEATURE_D3D12_OPTIONS6` (`vrs_tier`, `vrs_tile_size_px`, `vrs_additional_rates_supported`) and `ID3D12GraphicsCommandList5` at device initialization.
  - Supports three runtime modes (`VrsMode`: `off`, `uniform_2x2` Tier 1 per-draw `D3D12_SHADING_RATE_2X2`, and `foveated` Tier 2 `DXGI_FORMAT_R8_UINT` screen-space shading-rate image in `D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE` with `1x1` center fovea, anisotropic `2x1`/`1x2` mid-ring, `2x2` outer ring, and `4x4` far corners, rebuilt cleanly on swap-chain resize; automatic fallback to Tier 1 or `off` when unsupported).
  - Applies VRS strictly to the offscreen G-buffer scene pass (`scene_command_list5_`) while keeping late synthesis, reticle, and 2D HUD compositing on `present_command_list_` at full `1x1` resolution.
  - Added hotkey `B` to cycle VRS modes, CLI `set <pid> vrs off|uniform_2x2|foveated`, diagnostic overlay `set <pid> view vrs` showing the tile rate map, and `vrs` telemetry in `status` and `trace`.
- Verified zero-warning ARM64 Release and Debug builds (including live Debug D3D12 Debug Layer verification switching `off`, `foveated`, and `uniform_2x2`) and controlled 100-frame Release measurements on `NVIDIA RTX Spark N1X` (`1264x681`, `120 Hz` DWM deadline lock, `occlusion_sweep`, Tier 2 `tile_size_px: 16`, `additional_rates_supported: true`):
  - `combined /2`, `stress=64` (heavy shader load):
    - `vrs=off`: `scene_gpu_us` median **14,605 µs**, `cpu_source_age_us` median **34,431 µs**, `invalid_pixel_ratio` mean **0.630%**, 3 missed deadlines.
    - `vrs=foveated` (Tier 2): `scene_gpu_us` median **5,116 µs** (**2.85x GPU speedup**), **0 missed deadlines**, `cpu_present_interval_us` median **8,333 µs**, `cpu_source_age_us` halved to **17,495 µs**, and `invalid_pixel_ratio` halved to **0.299%** because scenes finish within their `/2` budget instead of slipping extra display intervals.
    - `vrs=uniform_2x2` (Tier 1): `scene_gpu_us` median **3,762 µs** (**3.88x GPU speedup**), `cpu_source_age_us` median **17,368 µs**, `invalid_pixel_ratio` mean **0.411%**.
  - `full_render`, `stress=48`: `vrs=off` **3,465 µs** → `vrs=foveated` **1,227 µs** (**2.82x speedup**, 0 missed deadlines).
- Honest limitations: `Motion Lab` is pixel-shader bound (raymarching + stress loop), so VRS produces near-theoretical pixel shading savings here; geometry- or bandwidth-bound workloads will see smaller gains, and coarse periphery rates (`2x2`/`4x4`) coarsen peripheral depth/motion edges (which `foveated` mitigates at screen center by keeping a `1x1` fovea).

## v0.12.0 — Motion-vector frame generation, temporal history recovery, and combined generation + late warp

- Added two new synthesis presentation modes in `src/runtime_state.h` (`runtime_state_version = 11`), `src/renderer.h`, `src/renderer.cpp`, `shaders/present.hlsl`, `src/main.cpp`, and `src/cli.cpp` (`schema_version: 7`):
  - `frame_gen` (`FrameKind::generated`, hotkey `4`, CLI `set <pid> mode frame_gen`): 5-tap foreground-nearest motion dilation + 2-step iterative backward motion-vector extrapolation from `T_n` scaled by `extrapolation_alpha = clamp(source_age / source_period, 0.0, 1.75)`, followed by depth-gated temporal history recovery from `T_{n-1}` (`prev_color_texture` / `prev_depth_texture` bound via the 3-slot source ring).
  - `combined` (`FrameKind::combined`, hotkey `5`, CLI `set <pid> mode combined`): 4-iteration depth-guided late camera warp toward the just-in-time target pose, layered with analytical static-camera motion subtraction to isolate and extrapolate independently moving objects (e.g., the orbiting sphere), plus depth-gated temporal color/depth history recovery from `T_{n-1}` for disocclusion holes.
- Extended the `reprojection_stats` UAV (`register(u1)`) to record both unrecovered holes (`invalid_pixel_ratio`) and temporally recovered pixels (`temporal_recovered_ratio`), highlighted in magenta (unrecovered) and teal (recovered from `T_{n-1}`) in `view=holes`.
- Verified zero-warning ARM64 Release and Debug builds (plus Debug smoke test) and live 120-frame Release comparisons on `NVIDIA RTX Spark N1X` (`1264x681`, `120 Hz` DWM deadline lock, `/4` divider, `stress=24`, `scene_gpu_us` ~5,633 µs, 30–35 async overlaps per 120 frames):
  - `occlusion_sweep`:
    - `reproject`: 0 missed deadlines, `cpu_present_interval_us` median **8,333 µs** (p95 8,502 µs), `present_gpu_us` median **151 µs**, `invalid_pixel_ratio` mean **0.808%**, `temporal_recovered_ratio` 0.000%.
    - `frame_gen`: 0 missed deadlines, `cpu_present_interval_us` median **8,333 µs** (p95 8,486 µs), `present_gpu_us` median **85 µs**, `invalid_pixel_ratio` mean **0.055%**, `temporal_recovered_ratio` mean **0.016%**.
    - `combined`: 0 missed deadlines, `cpu_present_interval_us` median **8,333 µs** (p95 **8,333 µs**), `present_gpu_us` median **232 µs**, `invalid_pixel_ratio` mean **0.675%**, `temporal_recovered_ratio` mean **0.023%** (max 0.066%).
  - `fast_pan`:
    - `reproject`: 1 missed deadline, `present_gpu_us` median **149 µs**, `invalid_pixel_ratio` mean **2.062%**.
    - `frame_gen`: 0 missed deadlines, `present_gpu_us` median **89 µs**, `invalid_pixel_ratio` mean **2.258%**, `temporal_recovered_ratio` mean **0.120%** (max 0.252%).
    - `combined`: 0 missed deadlines, `present_gpu_us` median **228 µs**, `invalid_pixel_ratio` mean **2.670%**, `temporal_recovered_ratio` mean **0.019%**.
- Honest limitations: `frame_gen` extrapolation relies on constant-velocity motion vectors and has weaker consistency checks than full 3D depth round-trip reprojection; during rapid `fast_pan`, newly exposed viewport edges are absent from both `T_n` and `T_{n-1}` (motivating future oversized render guard bands). NVIDIA DLSS-G via Streamline remains a separate future integration.

## v0.11.0 — Dual-queue asynchronous GPU execution and 3-slot source history ring

- Refactored `src/renderer.h` and `src/renderer.cpp` from a single serialized direct command queue into two independent D3D12 command queues: a `D3D12_COMMAND_QUEUE_PRIORITY_NORMAL` `scene_queue_` for heavy G-buffer rendering and a `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` `present_queue_` (with automatic NORMAL fallback) for late reprojection, HUD compositing, and `Present()`, each with its own fence, event handle, command list, and GPU timestamp query heap.
- Replaced the single offscreen G-buffer with a 3-slot source ring (`source_slots_[3]`) tracking `latest_completed_slot_` (`T_n`) and `previous_completed_slot_` (`T_{n-1}`) with per-slot `scene_fence_value` and `last_read_present_fence` lifetime guards so `scene_queue_` can render into an idle slot concurrently while `present_queue_` warps and presents the latest completed source without stalling on the scene pass.
- Wired non-blocking async scene dispatch (`async_gpu_enabled`, default `on`, hotkey `G`, CLI `set <pid> async_gpu on|off`) in `src/main.cpp`, `src/runtime_state.h` (`runtime_state_version = 10`), and `src/cli.cpp`, reporting `high_priority_present_queue` and `async_overlapped_presents` / `async_overlaps` in `status` and `trace`.
- Verified zero-warning ARM64 Release and Debug builds (plus Debug smoke test with D3D12 debug layer) and a live Release Sync vs. Async workload-spike comparison on `NVIDIA RTX Spark N1X` (`1264x681`, `120 Hz` DWM deadline lock, `reproject /2`, `stress=64`, 160-frame traces):
  - `high_priority_present_queue: true` was granted by D3D12 on RTX Spark N1X.
  - Synchronous (`async_gpu=off`): `async_overlaps: 0`, **80 / 160 missed deadlines**, `cpu_present_interval_us` median **10,900 µs** (min 2,082 / p95 13,292 / max 14,584 µs), `scene_gpu_us` median 4,574 µs, `present_gpu_us` median 47 µs.
  - Asynchronous (`async_gpu=on`): **81 overlapped presents**, **1 / 160 missed deadlines**, `cpu_present_interval_us` median **8,333 µs** (min 7,806 / p95 8,475 / max 8,719 µs), `scene_gpu_us` median 4,864 µs, `present_gpu_us` median 48 µs (p95 130 µs).
- Honest trade-off and limitations: non-blocking presentation from the previously completed source slot increases `cpu_source_age_us` median from ~10.8 ms to **17.4 ms** (~2 refresh intervals), which motivates motion-vector extrapolation and temporal history recovery in `v0.12.0`. Physical display scanout timing still requires elevated PresentMon/FrameView capture.

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
