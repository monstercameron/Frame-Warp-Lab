# Frame Warp Lab: initial research

Research started 26 November 2026. The aim is to investigate steady, responsive display updates even when a complete scene cannot be rendered at the monitor's refresh rate. This is a research hypothesis, not a validated renderer.

The primary question is whether **frame generation and asynchronous warp/reprojection together** can deliver perfectly paced output at the target monitor's upper refresh bound, with minimal image degradation and low latency. The individual techniques and ordinary presentation are experimental controls; the combined path is the main result. Test against the monitor's verified upper refresh bound, rather than selecting a lower target to make the result look successful. Perfect pacing and quality are hypotheses to measure, not promises.

For each run identify the monitor, output, resolution, highest supported refresh at that resolution and active refresh as exact rational values where available, plus VRR and presentation settings. The maximum refresh may depend on resolution, cable, display configuration and driver. Attempt to select and verify the relevant upper-bound mode; if it is unavailable, cannot be verified, or is not active, record the reason and do not label that run a valid upper-bound result. VRR runs must still target the same upper bound and report observed display intervals. Log changes in display configuration during a run and invalidate or segment affected results.

## The fundamental distinction

A rendered scene frame, a generated image, and a display refresh are different things. Generating intermediate images can make motion look smoother, but it does not necessarily produce a new simulation update or reduce input latency. Perfect output on every refresh regardless of GPU strength cannot be guaranteed: rendering, synthesis, memory traffic, compositing, scheduling and display all consume finite time. A 60 Hz frame has 16.67 ms; 120 Hz has 8.33 ms; 144 Hz has 6.94 ms; and 240 Hz has 4.17 ms. Missing visual information and missed deadlines are real constraints.

## Approaches

**Late reprojection:** retain a completed scene's color, depth and camera transform. Close to presentation, use a newer camera transform to project that image into a new view. Rotation-only warp is a simpler starting point; depth permits translation, but cannot reveal surfaces that were hidden in the original image. Moving objects, particles, transparency and UI need special handling. Large motion and old frames increase error. Initially expose holes with a diagnostic mask and provide a repeat-frame fallback.

**Interpolation:** estimate an intermediate image from two known frames, usually aided by motion and depth. Waiting for a later frame introduces a latency tradeoff. **Extrapolation** predicts from past information without waiting for that later frame, but is vulnerable to sudden motion, collisions, disocclusions and wrong predictions. Neither can reconstruct all information that was never rendered.

**NVIDIA DLSS Frame Generation:** NVIDIA's Streamline guide describes a proxy swap chain that intercepts presentation and handles frame generation and pacing. Integration requires coherent depth, dense motion vectors, a HUD-less scene image and Streamline Reflex. Separate UI data can improve UI handling. Support must be checked at runtime, and generated frames can be dropped. Multi Frame Generation and dynamic modes depend on hardware and system configuration; the guide states dynamic mode is currently limited to D3D12. Use a fallback when unsupported.

Streamline is mostly open source; the DLSS-G plugin is distributed as prebuilt binaries. The repository documents ARM64 build options for Streamline, but that alone does not establish that DLSS-G binaries or the full integration support this Windows ARM64 machine. Check actual release artifacts and the runtime feature query. Do not assume a custom presentation loop can be combined with DLSS-G: first test each mode independently and follow the SDK's swap-chain ownership rules.

**Multithreading:** separate input/simulation, scene preparation and presentation. Use bounded queues, frame IDs, explicit resource ownership and GPU fences; prefer a recent completed frame instead of accumulating a backlog. CPU threads do not create GPU capacity. A warp submitted on a different thread or queue can still miss its deadline if rendering saturates the GPU or scheduling prevents timely execution. Measure queue overlap and preemption on the actual hardware.

## Candidate architecture

1. Input and simulation produce timestamped camera and world snapshots.
2. The scene renderer produces color, depth, motion vectors, camera matrices and a frame ID into a small ring of GPU resources, then signals completion.
3. A presentation controller chooses the latest completed source without waiting for a new scene frame and presents either a repeated image or a late-warped image using the newest suitable camera pose. Record fallback and missed deadlines.
4. An optional Streamline backend uses NVIDIA's supported presentation path, runtime feature checks and Reflex. Treat it as a separate mode until compatibility with a combined design is established.
5. Telemetry records source age, simulation and render timings, displayed intervals, queue contention, fallback frequency and latency indicators.

This is a design to test, not a promise of independent GPU execution. Resource transitions, fences, camera conventions, resize, device loss and swap-chain behavior need careful implementation.

## Combining the techniques: hypothesis to test

The target is a responsive stream of display updates, not just a high generated-frame count. The proposed layers are: sample input and simulate frequently; render complete scene frames at a sustainable rate; optionally synthesize intermediate object motion; apply a small late camera warp using the freshest suitable pose; and composite the UI as late as the presentation path allows. Color, depth, motion vectors, camera transforms and timestamps must correspond to the same source frame. This ordering is a hypothesis: NVIDIA DLSS Frame Generation controls part of presentation, so a custom late warp cannot be assumed to fit after it. Establish whether the SDK supports a particular combination before implementing or claiming it.

### Timing and quality tradeoffs

- Protect real-frame throughput and latency first. Use a bounded queue, avoid building a backlog, and aim to leave GPU time for presentation. Separate threads or queues do not guarantee independent GPU execution; measure contention and scheduling under load.
- Camera reprojection is attractive for recent view changes, particularly small rotations over static geometry. Translation causes disocclusions; moving objects, particles, transparency and old depth can produce errors. Clamp excessive warp and visualize invalid areas.
- Interpolation can improve independently moving objects but normally uses information from two scene frames, introducing a possible wait and latency cost. Extrapolation avoids waiting for a future frame but can predict motion incorrectly. Compare responsiveness and quality as well as smoothness.
- Render a reticle and UI separately and late where the selected presentation path supports it. Log their update timing separately from the 3D scene; do not assume the overlay makes the world state current.
- NVIDIA frame generation requires a supported runtime, matching inputs and its documented Reflex and presentation integration. Test it independently before exploring a combined path. Do not assume the custom warp can operate on NVIDIA-generated frames or take ownership of its swap chain.

For example, 120 Hz provides a display opportunity every 8.33 ms. A sustainable 60 FPS full-render source produces a new scene frame about every 16.67 ms, leaving an opportunity to test an intermediate output. This is an illustrative cadence, not a guarantee: synthesis itself costs time, and actual pacing, latency and artifacts must be measured.

### Adaptive policy to investigate

Implement controls incrementally rather than assuming a finished automatic controller:

1. Cap the number of frames in flight and select the newest *completed* source frame; record its age. Do not block the presentation thread waiting for a new full frame.
2. Measure GPU timings and display deadlines. Adjust internal render resolution or scene quality to protect a useful base frame rate and allow headroom for any warp. Calibrate on the target hardware.
3. Set configurable maximum source age and warp displacement. On stale frames, camera cuts, invalid depth or missing history, skip the unsafe warp and present a known valid fallback. Record the reason.
4. When synthesis threatens the deadline, test a reduced-cost warp or repeat the last safe image. A missed or repeated display interval must be counted, not hidden in an FPS average.
5. If NVIDIA frame generation is supported, use its own documented pacing and capability checks, and compare its quality and latency as a separate mode. Determine independently whether a combined path is supported.

Very low real-frame rates limit the freshness of simulation, object motion and newly visible content. No synthesis mode can guarantee correct, responsive output for arbitrary load or GPU strength.

### Additional techniques to investigate

Keep these as individually measurable additions to the main combined frame-generation and asynchronous-reprojection experiment:

1. **Late input sampling and camera updates:** sample input close to the warp deadline and compare with input sampled at full-frame start. Keep simulation and camera state consistent. Measure input latency and prediction error; a newer camera pose does not mean the rest of the world has updated.
2. **GPU headroom and deadline-aware scheduling:** reserve capacity for generation and presentation, bound queues, and investigate shorter GPU workloads and scheduling under steady load and spikes. Measure actual GPU overlap and missed deadlines; separate threads or queues alone do not guarantee timely execution.
3. **Dynamic resolution and quality:** adjust scene-rendering cost to protect a useful real-frame rate and leave synthesis headroom. Compare scaling softness and temporal artifacts as well as pacing and latency. Record every quality or resolution change.
4. **Depth-aware reprojection confidence:** mark likely unreliable areas such as disocclusions, depth edges, fast-moving objects and transparency. Compare hole filling, conservative blending and presenting the last valid image. Record invalid-pixel coverage and visible artifacts.
5. **Motion-vector-assisted object prediction:** use object motion data alongside camera motion to investigate independently moving objects. Test sudden reversals, occlusion and missing or incorrect vectors; predictions cannot reliably anticipate every change.
6. **Separate UI and viewmodel treatment:** investigate late rendering or composition for reticles, menus, hands and weapons. Measure alignment with the world and actual game state: a responsive overlay over stale scene information can mislead.
7. **Temporal image reuse:** evaluate a short history of color and depth to fill newly exposed regions. Use the occlusion gate to compare reduced holes against ghosting, stale geometry and extra GPU cost.
8. **Adaptive generation policy:** select the amount and type of synthesis using frame age, motion, measured GPU headroom and deadline risk. Explain each decision through telemetry and compare with fixed policies; verify NVIDIA SDK pacing and ownership constraints before applying any policy to its path.
9. **Variable rate shading (VRS):** use D3D12-supported shading rates to reduce shading work in selected regions while preserving more detail where it matters. VRS changes shading frequency, not necessarily the resolution of the entire image or every rendering pass. Query the actual device's VRS tier and supported rates; expose unsupported status and keep a full-rate fallback. Compare uniform full-rate shading against controlled per-draw or image-based shading where the hardware supports it. Investigate masks based on screen position, motion and image features, with conservative rates around text, thin edges, UI and disocclusions. UI should remain separate where practical. Do not assume VRS accelerates a geometry-, bandwidth- or presentation-bound workload.

Start with the combined generation-and-warp path and its single-technique controls, then add late input sampling and GPU headroom one at a time. Follow with dynamic resolution and confidence-based artifact handling. Keep workload, camera path and the verified monitor upper-bound target fixed when isolating each change; investigate further combinations after their individual effects are understood.

Map a **quality envelope** across real-frame rates, workload and camera motion: identify where the combined system meets predefined display-pacing, latency and image-quality thresholds at the monitor's upper refresh bound, and where it fails. Define thresholds and measurement methods before evaluating results; report uncertainty, unsupported combinations and missing data.

For VRS, compare combined generation and warp with VRS off and on at the same scene, display upper-bound target, workload and resolution. Record GPU time per pass, supported tier and rates, shading-rate distribution, source-frame rate, presentation intervals, latency, and matched-reference image quality. Inspect flicker, edges, motion, fine text and disocclusions; VRS artifacts may interact with generated frames and warp. First test fixed, reproducible shading-rate patterns, then evaluate adaptive masks as a separate change. If support or timing data is unavailable, report it rather than infer a gain.

### Controlled comparison

Run the deterministic Motion Lab path in five modes: full-render baseline, lower-rate frame repetition, late camera reprojection, NVIDIA frame generation if supported, and the **combined frame-generation plus asynchronous-warp path** when a valid integration is established. The first four isolate contributions and provide references for the main combined experiment. If NVIDIA's presentation ownership prevents a documented combined integration, explicitly report that blocker and investigate supported integration or another frame-generation implementation; never silently substitute a single technique. Match workload, camera path, resolution and upper-bound display settings as closely as practical; benchmark modes independently because displaying multiple views can itself change GPU load. Where a high-rate reference is used for image comparisons, match scene state and timestamps. Record differences in configuration and feature availability.

For each run capture: the target refresh interval derived from the verified upper bound; actual displayed intervals and their distribution; missed, repeated and generated display updates where observable; real scene frame times; source age at display; CPU and GPU timings for render, generation and warp; queue depth; fallback rate and reasons; and input-to-display latency where observable. Inspect disocclusion holes, motion errors, ghosting, transparency, UI and camera-cut behavior against matched reference images. Report latency and image quality alongside pacing. Compare the combined mode with both single-technique controls at the same workload and display target. Define numerical quality and latency thresholds before claiming they are met; an average FPS equal to the target alone is not evidence of perfect pacing. External instrumentation or a high-speed camera is needed for physical input-to-photon validation.

## API decision

On 26 November 2026, we chose **Direct3D 12** for the first renderer. Vulkan remains a possible future backend, but D3D12 is the implementation target for this experiment. Verify the native Windows ARM64 compiler, Windows SDK, graphics tooling and NVIDIA runtime support before relying on any optional integration.

## First experiment

Build a native Windows D3D12 test scene with a movable camera, static geometry, moving objects, foreground occluders and separate UI. First establish ordinary rendering and frame repetition, then add depth-based camera reprojection with a visible invalid-pixel mask and pursue the combined generation-and-warp path. Inject controllable rendering load and test several source rates and resolutions, including overload; for every chosen resolution, use the verified upper refresh bound.

Use deterministic camera paths and repeatable inputs. Test sudden turns, moving objects, camera cuts and newly exposed surfaces. Compare against a high-rate reference at matching timestamps, while recognizing that image error is only a quality proxy. Add DLSS-G as an independently validated comparison if the SDK binaries and runtime support it.

Record separately: full scene FPS and frame-time distribution; actual displayed intervals and missed refreshes; source-frame age; input-to-display latency where measurable; CPU and GPU time for scene, warp and presentation; and artifacts such as holes, ghosting and unstable UI. Record GPU, driver, OS, monitor refresh, resolution, VSync/VRR state and capture method. A software timestamp is not a physical input-to-photon measurement; external measurement or a high-speed camera is needed for that.

PresentMon can capture Windows CPU, GPU and display-related metrics, but documents limitations including GPU timing accuracy with hardware-accelerated GPU scheduling. NVIDIA recommends FrameView's displayed-change metric for DLSS-G pacing because present-call timing alone can be misleading.

## Core architectural requirements

In addition to the ten prioritized experiments below, the renderer and measurement harness must be built around five foundational mechanisms:

- **Deadline-based scheduling rather than relying solely on VSync:** drive presentation from explicit rational refresh deadlines so pacing remains predictable and overruns are measurable.
- **Independent scene-rendering and reprojection queues:** decouple full-scene GPU work from late reprojection/presentation queues so heavy render spikes do not stall presentation.
- **GPU execution headroom reservation:** reserve measurable GPU capacity so generation and late warp can finish before the display deadline even under sustained load.
- **Actual displayed-frame timing through PresentMon:** validate physical displayed intervals, drops, and PC latency (`MsBetweenDisplayChange`, `MsUntilDisplayed`, `MsPCLatency`) rather than relying on `Present` return cadence alone.
- **Independent camera and world-state update frequencies:** allow late camera pose sampling and reprojection at display cadence while authoritative world simulation and full scene rendering run at their own sustainable cadence, reporting both update rates and latencies separately.

## Prioritized experiment roadmap

Execute the following ten experiments in priority order, validating each step with controlled measurements and recording limitations before advancing:

| Priority | Experiment | Purpose |
| :--- | :--- | :--- |
| 1 | Physical display timing instrumentation | Establish whether every refresh actually contains a useful update. |
| 2 | Independent rendering and warp scheduling | Prove that reprojection survives render workload spikes. |
| 3 | Motion-vector extrapolation | Update moving objects independently of camera movement. |
| 4 | Temporal color and depth history | Recover disoccluded surfaces. |
| 5 | Oversized render guard bands | Reduce missing information during fast camera rotations. |
| 6 | Independent frame-generation implementation | Retain control over synthesis and presentation ordering. |
| 7 | VRS and dynamic resolution | Protect rendering and warp deadlines. |
| 8 | Adaptive deadline-aware scheduler | Select rendering, generation, warp or fallback dynamically. |
| 9 | 1080p, 1440p and 4K benchmarks | Establish scaling characteristics. |
| 10 | Complex game-like workloads | Validate practical viability. |

## Machine observation

On 26 November 2026, `nvidia-smi` and the D3D12 capability query reported an `NVIDIA RTX Spark N1X (6144-core Blackwell RTX GPU)` in WDDM mode (driver `621.40`, `98290 MiB` dedicated VRAM), `DXGI_FEATURE_PRESENT_ALLOW_TEARING` supported, and D3D12 Variable Rate Shading **Tier 2** (`additional_shading_rates_supported: true`, `tile_size_px: 16`). Native ARM64 NVIDIA binaries are present on this machine: `C:\Windows\System32\nvapia64.dll`, `C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\FvSDK_arm64.dll` (and `SDK\Public_Release\ARM64\FvSDK_ARM64.lib`), `PresentMon_arm64.exe`, and `nvrla.exe`; NVIDIA Streamline `v2.14.16` also publishes official `streamline-sdk-v2.14.16-aarch64.zip` and `arm64ec` releases (DLSS-G runtime plugin behavior on this GPU remains to be tested). Active Windows display mode on `\\.\DISPLAY6` enumerated as `2880x1800` at nominal `120 Hz` (enumerated max `120 Hz` at active resolution; physical monitor upper bound and displayed scanout timing remain unverified). Local Python is ARM64. Measured Release findings on `1264x681` Motion Lab (`v0.10.0`–`v0.14.0`):
- **Dual-queue async execution (`v0.11.0`)**: D3D12 granted `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` for `present_queue_` alongside `NORMAL` `scene_queue_`; under `reproject /2`, `stress=64`, `120 Hz` deadline lock, missed deadlines dropped from **80/160 (sync)** to **1/160 (async, 81 overlaps)** with `cpu_present_interval_us` median **8,333 µs** and `present_gpu_us` median **48 µs** (trading ~1 extra frame of source age, 10.8 ms → 17.4 ms).
- **Frame generation & combined mode (`v0.12.0`)**: motion-vector extrapolation `frame_gen` ran in **85–89 µs** GPU time and `combined` (depth camera warp + independent moving-object extrapolation + `T_{n-1}` temporal history recovery) ran in **228–232 µs**, reducing `occlusion_sweep` `/4` mean holes from 0.808% to 0.675% with 0 missed deadlines; rapid `fast_pan` still exposes ~2.1–2.7% out-of-frustum border pixels absent from both `T_n` and `T_{n-1}`, directly motivating oversized render guard bands.
- **Variable Rate Shading (`v0.13.0`)**: on heavy pixel-bound `combined /2`, `stress=64`, Tier 2 `foveated` VRS cut `scene_gpu_us` median from **14,605 µs → 5,116 µs (2.85x speedup)**, eliminated missed deadlines (3 → 0), and halved both source age (34.4 ms → 17.5 ms) and `invalid_pixel_ratio` (0.630% → 0.299%); Tier 1 `uniform_2x2` reached **3,762 µs (3.88x speedup)**.
- **Adaptive deadline-aware scheduler (`v0.14.0`)**: with explicit 25%+ GPU headroom reservation and VRS-gain-compensated hysteresis, a `240 Hz` (`4,166 µs`) `stress` `0 → 64 → 0` ramp escalated to Tier 1 (`foveated VRS`, `scene_gpu_us` 1,565 µs) in 26 ms, held `cpu_present_interval_us` median **4,166 µs** / p95 **4,167 µs**, and recovered cleanly to Tier 0 with zero oscillation (2 total decisions).

## Sources

- [NVIDIA Streamline repository](https://github.com/NVIDIA-RTX/Streamline): framework, binaries and build options.
- [NVIDIA DLSS Frame Generation programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md): buffers, support checks, Reflex, presentation ownership and pacing.
- [Intel PresentMon](https://github.com/GameTechDev/PresentMon): Windows frame timing and measurement limitations.
- [AMD FidelityFX Super Resolution 3.1.4](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/): another frame-generation integration reference; verify platform support separately.
- [Microsoft Direct3D 12 synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization): fence-based GPU resource coordination.

## Questions to resolve

- Which resolution and monitor should the first experiment target, and what is that configuration's verified upper refresh bound?
- Are native ARM64 DLSS-G binaries and the required runtime features available on this machine?
- Can a small warp consistently meet display deadlines under GPU saturation, and what image quality does it retain?
- What balance of smoothness, image quality and input latency is acceptable when source FPS is very low?
