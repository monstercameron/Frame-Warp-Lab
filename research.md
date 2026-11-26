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

## Shading and resolution technology priorities

| Technology | Priority for Frame Warp Lab |
| :--- | :--- |
| Hardware VRS | Essential |
| Adaptive VRS | Essential |
| Artifact-aware VRS | Essential |
| Full-resolution depth and motion | Essential |
| Selective regional repainting | High |
| Dynamic resolution | Optional fallback |
| Temporal upscaling | Optional |

## Pre-neural conventional rendering optimization roadmap

Before turning to generative AI or neural novel-view synthesis, exhaust conventional deterministic graphics techniques that exploit **spatial redundancy, temporal redundancy, geometric information, and perceptual importance** — moving away from whole-frame repainting toward multi-rate, surface-cached, artifact-driven scheduling.

### 1. Spatial and temporal rendering optimizations

| Technique | What it does | Potential impact | Priority |
| :--- | :--- | :--- | :--- |
| Adaptive VRS | Changes shading rates according to GPU pressure and image complexity | Very high | P0 |
| Checkerboard rendering | Shades alternating pixels and reconstructs the remainder | High | P1 |
| Temporal interleaved rendering | Updates different pixel subsets across successive refreshes | Very high | P0 |
| Tile-based selective rendering | Repaints only regions that require fresh information | Very high | P0 |
| Temporal supersampling | Accumulates subpixel detail across successive updates | High | P1 |
| Deferred shading | Decouples expensive shading from geometry visibility | Very high | P0 |
| Visibility-buffer rendering | Stores primitive identity and barycentric coordinates before shading | Very high | P0 |
| Adaptive LOD | Reduces geometric complexity according to visual importance and budget | High | P1 |
| Dynamic resolution | Reduces internal pixel count when other optimizations are insufficient | High | P2 |
| Texture sampling adaptation | Adjusts filtering and mip selection according to regional importance | Medium | P2 |

### 2. Temporally amortized decoupled subsystem frequencies

Different rendering subsystems do not need to run at the same frequency. Illustrative experimental target frequencies on a 240 Hz display (not measured optima):
- **240 Hz**: presentation, HUD / viewmodel, late camera correction, object transforms.
- **120 Hz**: geometry visibility.
- **60 Hz**: material shading, direct lighting.
- **30 Hz**: reflections, shadow maps.
- **15 Hz**: global illumination, volumetric lighting.
- **5 Hz**: static environment lighting.

| Technique | Benefit | Priority |
| :--- | :--- | :--- |
| Independent animation interpolation | Update object poses without expensive material shading | P0 |
| Viewmodel separation | Render first-person hands and interactive objects at high frequency | P0 |
| Asynchronous shadow updates | Avoid regenerating unchanged shadow maps | P1 |
| Cached direct lighting | Reuse lighting on unchanged surfaces | P1 |
| Cached indirect lighting | Amortize expensive global illumination | P1 |
| Incremental reflection updates | Distribute reflection rendering over several frames | P1 |
| Amortized volumetric effects | Update fog and volumetric lighting at lower frequencies | P2 |

### 3. Texture-space shading and persistent surface caching

Store expensive shading results against object surfaces rather than exclusively in screen space so camera motion can re-rasterize visibility at high frequency while reusing cached diffuse/material shading:

| Technique | Function |
| :--- | :--- |
| Texture-space shading | Store shading in object texture coordinates |
| Object-space shading cache | Reuse results across camera movements |
| Surface-space lighting cache | Cache illumination independently of visibility |
| Shader result memoization | Avoid repeating identical shading calculations |
| Dirty-region invalidation | Recompute only changed surfaces |
| Adaptive cache resolution | Allocate cache detail according to projected importance |
| View-dependent shading separation | Recalculate specular effects while retaining reusable diffuse lighting |

### 4. Deterministic artifact correction

Retaining a second layer of occluded geometry/color (**layered depth images**) supplies real disoccluded scene information behind foreground occluders without generative guessing:

| Technique | Artifact addressed | Priority |
| :--- | :--- | :--- |
| Guard-band rendering | Missing image boundaries during camera rotation | P0 |
| Layered depth images | Disoccluded background surfaces | P0 |
| Separate foreground/background layers | Foreground stretching and ghosting | P0 |
| Multi-frame temporal cache | Recover previously visible surfaces | P0 |
| Acceleration-aware extrapolation | Reduce constant-velocity prediction errors | P0 |
| Reactive masks | Prevent inappropriate temporal reuse | P0 |
| Depth-aware dilation | Small disocclusion holes | P1 |
| Push-pull reconstruction | Missing pixels across spatial scales | P1 |
| Edge-aware interpolation | Blurred geometry boundaries | P1 |
| Bilateral reconstruction | Color bleeding across depth edges | P1 |
| Forward splatting with depth testing | Resolve competing projected surfaces | P1 |
| Bidirectional reprojection | Validate correspondence between frames | P1 |
| Hierarchical motion estimation | Improve large motion reconstruction | P1 |
| Classical optical flow | Recover motion not captured by geometry vectors | P1 |
| Variance clipping | Reduce temporal ghosting | P1 |
| Depth-aware sharpening | Restore detail without amplifying boundaries | P2 |

### 5. Geometry, GPU execution, and lighting optimizations

- **P0 GPU execution**: packed G-buffers (keeping full-precision `R32_FLOAT` depth where reprojection accuracy requires it), shader execution specialization (implemented in `v0.16.0`), wave-level primitives (implemented in `v0.16.0`), asynchronous compute, and deadline-bounded GPU work batches.
- **P1/P2 geometry & culling**: GPU-driven rendering, mesh shaders / meshlets, hierarchical Z-buffer, frustum and temporal occlusion culling, bindless resources, indirect draws, FP16 shader arithmetic where precision allows, geometry impostors, and adaptive tessellation.
- **Non-neural lighting reuse (for complex scenes)**: clustered lighting, ReSTIR DI/GI, radiance caching, probe-based GI, screen-space lighting reuse, adaptive ray counts, temporal denoising, variable-rate ray tracing, and cached shadow visibility.

### 6. Artifact-benefit shader optimization scheduler

Treat rendering and correction passes as independently schedulable operations prioritized by value per GPU cost inside the remaining deadline budget:

`Priority = (Expected visual error reduction * Visual importance) / Estimated GPU cost`

Operations such as late camera warp, motion extrapolation, disocclusion recovery, geometry edge correction, selective material repaint, shadow correction, and temporal detail recovery are scheduled first while lower-priority refinements (e.g., reflection or indirect-lighting updates) defer cleanly when the painting budget is tight.

### 7. 20-step pre-neural experiment sequence

| # | Experiment | Architectural value | Current status (`v0.19.0`) |
| :--- | :--- | :--- | :--- |
| 1 | Full-resolution visibility independent of shading | Foundational | Implemented (`v0.19.0`, SM 6.0 `vis_classify_main` + packed `R32_UINT` visibility buffer) |
| 2 | Adaptive, artifact-aware VRS | Foundational | Partial (Tier 1/2 static foveated + adaptive tier ladder; STeFAS content-driven mask pending) |
| 3 | Tile-level confidence and age tracking | Foundational | Implemented (`v0.19.0`, per-pixel age + `16×16` wave-reduced tile status map + `view=tiles`) |
| 4 | Selective regional repainting | Foundational | Implemented (`v0.19.0`, GPU `ExecuteIndirect` repainting: **6.02×** faster on `corridor_sway`, **4.76×** on `occlusion_sweep`) |
| 5 | Deadline-bounded GPU work submission | Foundational | Implemented (`v0.19.0`, lock-free CAS `tile_budget` work admission with progressive deferral) |
| 6 | Guard-band rendering | Artifact reduction | Planned |
| 7 | Layered depth and color buffers | Artifact reduction | Planned |
| 8 | Rotation-only reprojection | Performance | Implemented (`v0.16.0`, 0 depth samples, 53 µs) |
| 9 | Acceleration-aware motion extrapolation | Temporal accuracy | Partial (constant-velocity MV extrapolation in `v0.12.0`; acceleration term pending) |
| 10 | Multi-frame history reconstruction | Artifact reduction | Partial (`T_{n-1}` 2-frame history in `v0.12.0`; deeper cache pending) |
| 11 | Temporal interleaved shading | Performance | Planned |
| 12 | Texture-space shading | Architectural | Planned (requires rasterized geometry scene) |
| 13 | Persistent surface shading cache | Architectural | Planned (requires rasterized geometry scene) |
| 14 | Independent subsystem update frequencies | Architectural | Partial (camera/HUD vs. scene decoupled; per-pass lighting rates pending) |
| 15 | Independent dynamic-object rendering | Responsiveness | Planned |
| 16 | Adaptive geometry LOD and culling | Performance | Planned (requires rasterized geometry scene) |
| 17 | Classical optical flow | Temporal accuracy | Planned |
| 18 | Temporally amortized lighting | Performance | Planned |
| 19 | Artifact-benefit-based shader scheduling | Architectural | Planned |
| 20 | Dynamic resolution as an emergency fallback | Resilience | Planned |
| 21 | Priority-bucketed perceptual tile scheduler (`P_i = E_i · S_i · A_i / C_i`) + `view=priority` | Perceptual scheduling (P0) | Implemented (`v0.20.0`, 8-bin GPU priority admission: `63.1% → 100.0%` critical coverage on `fast_pan 15%`) |
| 22 | 3D Spatiotemporal Blue Noise (STBN) tile/pixel refresh phasing | Perceptual masking (P0) | Implemented (`v0.20.0`, 16×16 void-and-cluster toroidal blue-noise phase LUT) |
| 23 | Hybrid contiguous (`16×16` disocclusions) + `4×4` sub-tile interleaved shading + `3×3` sliding window | High-Hz integration (P0) | Planned (`v0.21.0`, Andersson et al. 2019) |
| 24 | TCSF-clamped Temporal Resolution Multiplexing (TRM) detail compensation on repainted/warped frames | Sharpness recovery (P1) | Implemented (`v0.21.0`, 5-tap clamped Catmull-Rom + TCSF TRM: `+2.30 dB` PSNR and `74.9% → 90.9%` sharpness on `fast_pan 10%`; `101.0%` sharpness at `15%`) |
| 25 | Authoritative vs. synthesized history firewall (anti-recursive-drift enforcement + PatchEX-style fg/bg repair) | Temporal stability (P0) | Partial (`v0.19.0`–`v0.23.0` enforces authoritative `SourceSlot` cache vs read-only synthesis; long-run drift audit & fg/bg layer separation pending) |
| 26 | Matched-reference PAR quality & perceptual test matrix (Modes A–F: `240 Hz` native vs `5%/10%/20%` patch budgets, static vs smooth pursuit) | Validation (P0) | Partial (`v0.21.0`–`v0.23.0` automated exact-timestamp PSNR/SSIM/Sharpness/P99 harness proved `15%` PAR beats VRS Tier 2 & `50%/67%` spatial upscaling; multi-second temporal video harness, FovVideoVDP & blinded human tests pending) |
| 27 | Damage-rate-aware scheduler (preventing sustained repair backlog + camera-cut budget bursts) | Resilience (P0) | Partial (`v0.21.0`–`v0.22.0` auto-budgeter + `age=255` provenance + `4×4` coarse fallback; explicit damage-rate floor & cut-burst admission pending) |
| 28 | Multi-resolution scaling (`1080p`, `1440p`, `4K`) & offscreen upscaling | Scaling (P0) | Proven (`v0.23.0` offscreen `1080p`/`1440p`/`4K` scaling + spatial upscaling vs PAR; temporal upscaling `TAAU`/`DLSS-SR` pending) |
| 29 | Temporal video quality harness (multi-second motion sequence & FovVideoVDP) | Validation (P0) | Planned |
| 30 | Post-synthesis painting (repairing residual artifacts after warp/frame-gen) | Artifact reduction (P1) | Planned |

### 8. Recommended experiments before complex shaders

Before expanding to complex rasterized mesh/PBR shaders, validate the architecture through these 10 gated experiments (mirrored in `experiment-plan.md`):

| Priority | Experiment | Passing criterion | Current status (`v0.23.0`) |
| :---: | :--- | :--- | :--- |
| 1 | PAR-3 sub-tile interleaving | Better temporal quality at equivalent painting cost | Planned |
| 2 | PAR-5 history firewall | No unbounded recursive degradation | Partial (authoritative cache invariant enforced; long-run drift test & fg/bg separation pending) |
| 3 | Damage-rate-aware scheduler | No sustained repair backlog under feasible load | Partial (deadline auto-budgeter + `age=255` + `4×4` coarse fallback done; damage-rate floor pending) |
| 4 | Independent update frequencies | Stable output with reduced visibility and shading frequencies | Partial (camera/HUD vs. scene decoupled; decoupled visibility/shading/lighting rates pending) |
| 5 | Multi-resolution scaling | Predictable performance at 1080p, 1440p and 4K | Proven (`v0.23.0`–`v0.24.0` zero-alloc per-frame DRS up/down + `1080p`/`1440p`/`4K` scaling & stacked `4K` DRS+PAR `12.24×`) |
| 6 | Temporal video quality harness | Quality measured across entire motion sequences | Planned |
| 7 | Physical display measurements | Verified displayed cadence and input latency | Planned (interactive FrameView / PresentMon run) |
| 8 | Foreground DLSS-G validation | Actual generated frames with acceptable quality | Partial (Streamline ARM64 `6×` MFG verified; interactive focused run pending) |
| 9 | Post-synthesis painting | Measurable improvement over uncorrected synthesis | Planned |
| 10 | Blinded perceptual A/B tests | Establish perceptual equivalence thresholds | Planned |

## Inverse-rendering architecture and verified literature backlog

In Frame Warp Lab's architectural sense, **inverse rendering** means fixing the presentation deadline first (`T_execution(S) <= T_deadline`) and treating image quality as the constrained optimization variable (`minimize E_perceptual + lambda * E_temporal`) across six decoupled layers:

1. **Display clock**: fixed refresh cadence and rational presentation deadlines.
2. **Real-time compositor**: latest input, late camera correction, and frame delivery.
3. **Persistent visual state**: multilayer depth, geometry, motion, and surface shading caches.
4. **Error evaluation**: confidence, disocclusion, tile staleness, and perceptual error proxies.
5. **Adaptive painting scheduler**: prioritized shader and tile-level rendering/correction operations.
6. **Authoritative scene renderer**: rasterization, ray tracing, materials, and lighting.

### Platform feasibility tiers (Windows 11 ARM64 + D3D12 SM 6.0 + RTX Spark N1X)

- **Tier 1 — Immediately feasible on the current D3D12 architecture (no scene rewrite)**:
  - **STeFAS content-adaptive VRS**: GPU SM 6.0 compute pass dynamically updating the Tier 2 `16×16` `R8_UINT` shading-rate image from edges, motion, and reprojection holes with temporal hysteresis.
  - **Confidence-driven redraw maps & selective tile repainting** (Time-Warped Foveated + Amulet): `16×16` tile error/age scoring, SM 6.0 wave compaction (`WavePrefixCountBits`), and `ExecuteIndirect` repainting of dirty tiles only.
  - **Amulet-style sparse multilayer cache & predictive tile lifetimes**: 2nd-hit occluded color/depth layer (`LDI`) plus per-tile expiration lifetimes to resolve disocclusions from real geometry instead of neural hallucination.
  - **Guard-band rendering**: +10–15% offscreen G-buffer margin with coarse border VRS to eliminate fast-pan/rotation viewport-edge holes.
  - **Deferred Adaptive Compute Shading (DACS)**: hierarchical variance-driven SM 6.0 compute shading (`4×4 → 2×2 → 1×1`) benchmarked directly against hardware Tier 2 VRS.
  - **Acceleration-aware extrapolation & reactive masks**: quadratic velocity+acceleration prediction using `T_n` / `T_{n-1}` motion history (with reversal clamping) plus reactive masks preventing stale reuse.
  - **Perceptual error validation**: offline Python FovVideoVDP evaluation on matched reference vs. synthesized frames paired with a lightweight runtime contrast/velocity/eccentricity priority proxy.
- **Tier 2 — Feasible after adding a conventional rasterized mesh scene (and optional DXR)**:
  - **Visibility-buffer rendering**, **FastAtlas / Shading Atlas Streaming / SnakeBinning** persistent texture-space shading caches, **adaptive geometric LOD / meshlets**, **reverse reprojection / multi-rate shader memoization**, **PatchEX** parallel patch extrapolation, **FAST** filter-adapted stochastic sampling, **stochastic texture filtering**, and **Multi-Layer Reservoir Splatting / Temporally Dense Ray Tracing** (via Blackwell D3D12 Raytracing Tier 1.1).
- **Tier 3 — Blocked on standard Windows 11 WDDM / consumer displays (reference only)**:
  - **Scanline-aware / beam-racing rendering** and **direct FPGA/hardware render-display scanout integration (<2 ms click-to-photon)**: not achievable on user-mode Windows WDDM `DXGI_SWAP_EFFECT_FLIP_DISCARD` swap chains without custom display/OS hardware.

### Priority research techniques added from literature

| Priority | Techniques |
| :--- | :--- |
| **P0** | Deadline-constrained work admission (done `v0.19.0`); frameless/progressive tile rendering (done `v0.19.0`); priority-bucketed perceptual tile scheduling (`P_i = E_i·S_i·A_i/C_i`, Dayal 2005 + Cater 2003); 3D Spatiotemporal Blue Noise (STBN) refresh phasing (Wolfe 2022); hybrid contiguous + `4×4` sub-tile interleaved shading with `3×3` sliding reconstruction (Andersson 2019); authoritative vs. synthesized history firewall (anti-recursive-drift); predictive GPU scheduling; temporally amortized rendering; decoupled visibility & shading (visibility buffer, done `v0.19.0`); confidence-driven redraw maps (done `v0.19.0`); tile-level selective repainting (done `v0.19.0`); STeFAS content-adaptive VRS; rotation-only reprojection (done `v0.16.0`); guard-band rendering; layered depth images (LDI); persistent render cache; Deferred Adaptive Compute Shading (DACS); texture-space shading; Amulet-style predictive tile lifetimes; matched-reference PAR validation harness (Modes A–F). |
| **P1** | TCSF-clamped Temporal Resolution Multiplexing (TRM) detail compensation (Denes & Mantiuk 2019); task-driven foveal/reticle saliency maps (Cater 2003); spatial-scatter velocity/frequency bounds (Zagier 1997); reverse reprojection caching; automated shader memoization; multi-rate material shading; FastAtlas surface-space shading atlases & dynamic packing (SnakeBinning); hierarchical sparse shading; acceleration-aware extrapolation; classical optical flow; PatchEX patch-based parallel extrapolation; foreground/background layer separation; forward depth-aware splatting; multi-frame temporal cache; temporally dense ray tracing; FAST filter-adapted stochastic sampling; stochastic texture filtering; ReSTIR & Multi-Layer Reservoir Splatting under disocclusion; perceptual error prioritization (FovVideoVDP offline + runtime proxy); independent lighting frequencies. |
| **P2 / P3** | Dynamic geometric LOD (`P2`); scanline-aware rendering (`P2`, WDDM-blocked); direct render-display hardware integration (`P3`, custom hardware required). |

## Perceptually Amortized Rendering (PAR) — Frameless & Human-Vision Literature Synthesis

Building on `v0.19.0`'s visibility-guided selective tile repainting, Frame Warp Lab's research direction converges on **Perceptually Amortized Rendering (PAR)** / **Deadline-Constrained Adaptive Frameless Rendering**: maintaining a continuously evolving, provenance-tracked framebuffer where reprojection updates camera geometry at display cadence (`120–480 Hz`), authoritative shading continuously repairs only the highest-perceptual-error regions within the remaining GPU deadline budget, and human spatiotemporal integration fuses rapid micro-updates into a coherent image.

### Concrete mechanisms extracted from primary literature

| Paper | Verified core mechanism | Direct idea to borrow for Frame Warp Lab | Honest boundary / failure mode |
| :--- | :--- | :--- | :--- |
| **Frameless Rendering** (Bishop et al., 1994) & **Defining and Refining Frameless Rendering** (Scher Zagier, UNC 1997) | Asynchronous randomized pixel updates from newest pose; Zagier formalized **spatial scatter** as a function of object/camera velocity × spatial frequency vs. pixel update rate, plus pixel aging. | Frame Warp Lab eliminates Zagier's 1997 spatial-scatter failure mode by running depth-guided Space Warp *before* selective painting (moving 85–92% of pixels coherently), leaving selective updates to repair only the residual disocclusion/new-detail error while enforcing age bounds (`A_i`). | Without coherent reprojection, pure stochastic painting degrades into visible edge snow during fast motion. |
| **Detail to Attention** (Cater, Chalmers & Ward, EGSR 2003) | Exploits task-driven **inattentional blindness**: viewers engaged in an active visual task fail to notice peripheral shading reductions outside their task focus. | Weight tile perceptual importance `S_i` around the center aiming reticle / active interaction zone and high-contrast foreground silhouettes, rather than treating all screen tiles equally. | Breaks down if the viewer freely scans the periphery instead of focusing on the active task target; must retain an age floor so peripheral tiles never go permanently stale. |
| **Adaptive Frameless Rendering** (Dayal, Woolley, Watson & Luebke, EGSR 2005) | Closed-loop sampler + k-d tile hierarchy guided by **spatio-temporal color variation**; deep temporal sample buffer with adaptive filter bank (long temporal accumulation on static regions, short recency weighting on moving regions); reported ~10× fewer samples at ~15% overhead. | **1)** Replace `v0.19.0`'s FIFO threadgroup work admission with a **GPU priority-bucketed scheduler** scoring `P_i = (E_i * S_i * A_i) / C_i` (disocclusion + depth-jump + spatial/temporal color gradient × foveal/task saliency × age urgency / cost) via a fast 4–8 bin GPU histogram + prefix sum so `tile_budget=10–20%` always repairs the worst visual errors anywhere on screen instead of top-left tiles first. **2)** Use adaptive temporal filtering (accumulate static tiles sharply; bias dynamic tiles toward newest ground truth). | High-frequency speculars and thin silhouettes still trigger large color gradients; k-d tree CPU/GPU tree maintenance is unnecessary on modern GPUs compared to fixed `16×16` / `4×4` compute buckets. |
| **Temporally Dense Ray Tracing** (Andersson, Nilsson, Salvi, Spjut & Akenine-Möller, NVIDIA HPG 2019) | Targets 240+ FPS by interleaving pixel shading inside small tiles (`4×4`) with **adaptive per-tile pixel ordering** and a localized `3×3` spatio-temporal reconstruction window (avoiding exponential-moving-average TAA ghosting). | **Hybrid contiguous + interleaved painting**: repaint hard disocclusions/moving silhouettes as contiguous `16×16` tiles (zero holes), while spreading background shading refreshes across `4×4` sub-tile interleaved subsets with a short sliding temporal window instead of sweeping full `16×16` blocks. | Fixed checkerboard/Bayer patterns produce visible crosshatch strobing on moving high-contrast edges; adaptive ordering + blue-noise phase distribution is required. |
| **Spatiotemporal Blue Noise Masks (STBN)** (Wolfe, Morrical, Akenine-Möller & Ramamoorthi, EGSR 2022) | 3D volume masks (`X, Y, Time`) optimized via void-and-cluster so both each 2D frame *and* each pixel's temporal sequence have blue-noise spectra (pushing error into high spatiotemporal frequencies where vision masks it). | Replace `v0.19.0`'s modular periodic-refresh hash `(x*7 + y*13 + frame) % 16` (which creates subtle diagonal refresh waves) with a **precomputed 3D STBN tile-phase mask** (`64×64×16`) so background tile/sub-tile refreshes are perceptually uniform with zero crawling bands or temporal clumping. | Blue noise masks error perceptually only when refresh frequency is high enough (`>= 120 Hz`); at low refresh (`<= 60 Hz`), dispersed updates still twinkle. |
| **Temporal Resolution Multiplexing (TRM)** (Denes, Maruszczyk, Ash & Mantiuk, IEEE TVCG 2019) | Alternates lower-resolution frames `F_i` with high-resolution **compensated frames** `F_{i+1} = 2I - F_i` so temporal average equals `I`, bounded by a **Spatio-Temporal Contrast Sensitivity Function (TCSF)** flicker limit and display EOTF response. | Test **TCSF-clamped temporal detail compensation** (`I + alpha * (I - F_cached)`) when freshly repainting cached/bilinearly-warped tiles or alternating coarse/fine frames at 120–240+ Hz to counteract bilinear reprojection softening without triggering visible flicker or luminance clipping. | Aggressive boost exceeds human flicker fusion thresholds (`TCSF` at `R/2`) on high-contrast bright edges and depends on panel response time (OLED vs. LCD). |
| **Continuous Temporal Integration (CTI)** (Deodato & Melcher, *Journal of Vision*, 2024, PMC11622157) | 50-participant continuous mouse-tracking study using flickering sine-wave gratings in dynamic noise under free viewing; measured an ~100 ms temporal integration window. | Supports investigating whether micro-imperfections resolved within 15–40 ms at 240–480 Hz fuse below conscious notice during continuous motion. | **Critical caveat**: a 100 ms integration window for noisy gratings does *not* imply 100 ms disocclusion holes or sharp geometric silhouettes are invisible in games; smooth pursuit and saccades expose structural errors in 10–25 ms. Must be tested directly via matched-reference error + blinded user trials. |
| **PatchEX** (Dixit & Sarangi, ACM TOG 2025) | Parallel foreground/background segmentation, motion-vector extrapolation, disocclusion masking, and lightweight wavelet-pruned patch inpainting to avoid interpolation latency. | Strictly separate **authoritative ground-truth history** (`SourceSlot` cache + visibility buffer) from **synthesized / DLSS-G display frames** — never feed DLSS-G output back into the persistent scene cache (preventing recursive geometry/lighting drift), and separate foreground moving silhouettes from background disocclusion repair. | Pure neural inpainting cannot recover unseen high-frequency geometry/text; ground-truth selective patch rendering must periodically reset accumulated error. |

### Architectural invariant: Preventing recursive error accumulation

- **Authoritative history (`SourceSlot` ring, `scene_queue_`)**: holds only ground-truth geometry visibility, exact camera/object motion, and selectively repainted ground-truth shading. Cached pixels carry explicit age (`0..255`) and are periodically reset by authoritative shading.
- **Synthesized display stream (`present_queue_` / Streamline DLSS-G)**: reads the authoritative history to synthesize intermediate display updates (`Space Warp`, `frame_gen`, `combined`, `dlss_g`, `dlss_g_warp`), and **never** writes synthesized pixels back into the authoritative history.

### Proposed PAR experiment sequence (`v0.20.0`+)

1. **`v0.20.0` — Priority-Bucketed Perceptual Tile Scheduler + STBN Refresh**: replace FIFO CAS admission in `selective_tiles.hlsl` with an 8-bin GPU priority histogram (`P_i = (E_i * S_i * A_i) / C_i`: disocclusion fraction + edge/color gradient × foveal reticle saliency × age urgency / cost) + prefix-sum work admission, and replace the modular periodic hash with a 3D Spatiotemporal Blue Noise (STBN) tile-phase LUT. Add a live `view=priority` heatmap.
2. **`v0.21.0` — Hybrid Sub-Tile Interleaved Shading (`4×4`) + TCSF-Clamped Temporal Compensation**: keep contiguous `16×16` tiles for hard disocclusions while offering `4×4` sub-tile interleaved shading (Andersson 2019) for smooth regions, plus optional TRM-style TCSF-clamped detail compensation (Denes & Mantiuk 2019) on freshly repainted tiles to counter bilinear cache softening.
3. **`v0.22.0` — Matched-Reference Quality Harness & Blinded PAR Matrix**: implement deterministic frame-capture comparison (PSNR / SSIM / FovVideoVDP vs. 240 Hz full-render ground truth) and automated A/B/C/D/E/F sweep comparing `240 Hz native`, `60→240 Hz warp`, and `30→240/480 Hz` with `5% / 10% / 20%` perceptually prioritized patch budgets across both static gaze and fast camera/eye motion.

## Machine observation

On 26 November 2026, `nvidia-smi` and the D3D12 capability query reported an `NVIDIA RTX Spark N1X (6144-core Blackwell RTX GPU)` in WDDM mode (driver `621.40`, `98290 MiB` dedicated VRAM), `DXGI_FEATURE_PRESENT_ALLOW_TEARING` supported, and D3D12 Variable Rate Shading **Tier 2** (`additional_shading_rates_supported: true`, `tile_size_px: 16`). Native ARM64 NVIDIA binaries are present on this machine: `C:\Windows\System32\nvapia64.dll`, `C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\FvSDK_arm64.dll` (and `SDK\Public_Release\ARM64\FvSDK_ARM64.lib`), `PresentMon_arm64.exe`, and `nvrla.exe`; NVIDIA Streamline `v2.14.16` ARM64 (`streamline-sdk-v2.14.16-aarch64.zip`) is integrated and verified live on this GPU. Active Windows display mode on `\\.\DISPLAY6` enumerated as `2880x1800` at nominal `120 Hz` (enumerated max `120 Hz` at active resolution; physical monitor upper bound and displayed scanout timing remain unverified). Local Python is ARM64. Measured Release findings on `1264x681` Motion Lab (`v0.10.0`–`v0.17.0`):
- **Dual-queue async execution (`v0.11.0`)**: D3D12 granted `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` for `present_queue_` alongside `NORMAL` `scene_queue_`; under `reproject /2`, `stress=64`, `120 Hz` deadline lock, missed deadlines dropped from **80/160 (sync)** to **1/160 (async, 81 overlaps)** with `cpu_present_interval_us` median **8,333 µs** and `present_gpu_us` median **48 µs** (trading ~1 extra frame of source age, 10.8 ms → 17.4 ms).
- **Frame generation & combined mode (`v0.12.0`)**: motion-vector extrapolation `frame_gen` ran in **85–89 µs** GPU time and `combined` (depth camera warp + independent moving-object extrapolation + `T_{n-1}` temporal history recovery) ran in **228–232 µs**, reducing `occlusion_sweep` `/4` mean holes from 0.808% to 0.675% with 0 missed deadlines; rapid `fast_pan` still exposes ~2.1–2.7% out-of-frustum border pixels absent from both `T_n` and `T_{n-1}`, directly motivating oversized render guard bands.
- **Variable Rate Shading (`v0.13.0`)**: on heavy pixel-bound `combined /2`, `stress=64`, Tier 2 `foveated` VRS cut `scene_gpu_us` median from **14,605 µs → 5,116 µs (2.85x speedup)**, eliminated missed deadlines (3 → 0), and halved both source age (34.4 ms → 17.5 ms) and `invalid_pixel_ratio` (0.630% → 0.299%); Tier 1 `uniform_2x2` reached **3,762 µs (3.88x speedup)**.
- **Adaptive deadline-aware scheduler (`v0.14.0`)**: with explicit 25%+ GPU headroom reservation and VRS-gain-compensated hysteresis, a `240 Hz` (`4,166 µs`) `stress` `0 → 64 → 0` ramp escalated to Tier 1 (`foveated VRS`, `scene_gpu_us` 1,565 µs) in 26 ms, held `cpu_present_interval_us` median **4,166 µs** / p95 **4,167 µs**, and recovered cleanly to Tier 0 with zero oscillation (2 total decisions).
- **Offline DXC SM 6.0, wave atomics & specialized PSOs (`v0.15.0`–`v0.16.0`)**: migrating to offline Windows SDK ARM64 `dxc.exe` (`vs_6_0`/`ps_6_0`, `-O3 -WX`), wave-level atomic reductions (`WaveActiveCountBits`), CPU-precomputed homography matrices, and 5 specialized PSOs cut `pure_rotation /4` warp time to **53 µs** (0 depth samples, **2.85x faster** than the 151 µs depth solver), `occlusion_sweep /4` depth warp to **104 µs** (31% faster), and `combined` synthesis to **209 µs** (10% faster) with 0 / 500 missed deadlines.
- **NVIDIA Streamline ARM64 Reflex + DLSS-G + Space Warp (`v0.17.0`–`v0.18.0`)**: native ARM64 Streamline `2.14.16` (`NGX 310.9.16`, Blackwell `m_gpuArch = 0x1b0`) initialized cleanly with `sl.interposer.dll` as sole DXGI/D3D12 import, reporting **`dlssg_supported: true`**, **`max_frames_to_generate: 5` (up to 6× Multi Frame Generation)**, **`dynamic_mfg_supported: true`**, **`dlss_sr_supported: true`**, `reflex_supported: true`, and `vsync_supported: true`. In `v0.18.0`, the 4-RTV `present_dlss_inputs_ps` pass generates target-camera-aligned `hudless`, `clip_depth`, and `motion` (including scaled independent object motion and `T_{n-1}` temporal recovery) into a 3-slot dual-fence-audited `DlssInputSlot` ring in **64–69 µs** GPU time while returning the G-buffer to 3 lean MRTs, with full `sl::Result` auditing (`api_error_count: 0`), explicit `pacing_owner` telemetry, a 1.5%-hole artifact safety guard (`warp_guard_trips` verified live on `fast_pan /6`), and a unified `AdaptiveScheduler` supporting DLSS-G + Space Warp tiers. Automated runs inside an isolated non-active Windows desktop (`ThreadDesktop=exebox-...`) report `window_focused: false` (`GetForegroundWindow() == 0`), where Streamline's `shouldInterpolate` gate intentionally pauses interpolation (`numFramesActuallyPresented: 1`); running `scripts/verify_dlssg_foreground.ps1` interactively on `WinSta0\Default` (alongside FrameView `MsBetweenDisplayChange`) is required to validate physical multi-frame scanout cadence.
- **Visibility-guided selective tile repainting + `ExecuteIndirect` work admission (`v0.19.0`)**: decoupling full-resolution visibility + temporal cache reprojection (`tile_vis_classify_cs`) from `ExecuteIndirect` `16×16` dirty-tile shading (`tile_shade_indirect_cs`) reduced `stress=64`, `120 Hz` `scene_gpu_us` median on `corridor_sway` from **4,621 µs (whole-frame)** and **1,619 µs (VRS Tier 2 foveated)** down to **767 µs (6.02× faster than native, 2.11× faster than VRS Tier 2)** while shading only **8.4% median** of tiles at full 1×1 native resolution, and cut `occlusion_sweep` from **4,605 µs → 967 µs (4.76× faster, 14.6% tiles)**. Even at `stress=0`, selective tiles (`420 µs`) beat whole-frame rendering (`441 µs`). Enabling `tile_budget` work admission capped `fast_pan` p95 at **1,365 µs** (`20.0%` tile ceiling, 0 missed deadlines) and `corridor_sway` p95 at **948 µs** (`tile_budget=15`). Composed with `reproject /4` at `240 Hz` (`stress=64`), the full inverse-rendering stack sustained **240.1 FPS (`ft_med: 4,166 µs`, 0 missed deadlines)** at **731 µs** scene GPU + **28 µs** warp (~**211 µs** amortized GPU cost per 240 Hz display frame, **~22×** lower than native whole-frame rendering), and composed cleanly with `dlss_g_warp` (`api_error_count: 0`, `8.6%` tiles shaded).
- **PAR-1 + PAR-2 Priority-Bucketed Scheduler, 3D STBN & Clean-Feed PAR + DLSS-G (`v0.20.0`)**: replacing FIFO tile admission with 8-bin GPU perceptual priority buckets ($P_i = E_i \cdot S_i \cdot A_i / C_i$) and a 16×16 toroidal void-and-cluster blue-noise refresh LUT lifted `critical_coverage_ratio` (disocclusions + moving silhouettes actually repaired) from **63.1% → 100.0%** on `fast_pan 15%` (`19,653 → 0` dropped critical tiles) and **84.5% → 100.0%** on `occlusion_sweep 10%` (`3,142 → 0` dropped) while running **30–132 µs faster** than FIFO (`1,061 µs` vs `1,106 µs`, `1,017 µs` vs `1,149 µs`). At `240 Hz` (`stress=64`, `corridor_sway`), continuous frameless `5%` patching ran in **567 µs** (`99.5%` critical coverage) and `10%` ran in **857 µs** (**100.0%** critical coverage). In Variant B (**Clean-Feed PAR + DLSS-G**, `fast_pan`, `stress=64`, `tile_budget=15%`, `async_gpu=off`), `scene_gpu_us` dropped **4.55×** vs. whole-frame `dlss_g_warp /4` (`5,288 µs → 1,163 µs`) while slashing Space Warp holes from **2.22% → 0.000%** at **100.0%** critical coverage and `api_error_count: 0`.
- **PAR-4 + PAR-6 Matched-Reference Quality Proof & Closed-Loop Auto-Budgeter (`v0.21.0`)**: using deterministic virtual stepping (`fixed_dt_us = 4167`, exact `t = 408,366 µs` Frame `#98` @ 240 Hz) and lossless `.ppm` backbuffer readback against `1×1` Ground Truth (`stress=32`, overlay off):
  - **PAR `15%` + 5-tap Catmull-Rom & TCSF TRM (`cache_sharpen=on`) beat Hardware VRS Tier 2 Foveated on every metric simultaneously**: on `occlusion_sweep`, **`36.62 dB` PSNR / `0.9730` SSIM / `103.5%` edge sharpness / `P99=20` in `382 µs`** vs. VRS Tier 2's **`32.54 dB` / `0.9438` / `P99=45` in `1,071 µs`** (`+4.08 dB` higher quality at `2.80×` faster GPU time!); on `fast_pan`, **`35.58 dB` / `0.9644` SSIM / `101.0%` sharpness / `P99=22` in `489 µs`** vs. VRS Tier 2's **`31.78 dB` / `0.9350` / `P99=44` in `943 µs`** (`+3.80 dB` higher quality at `1.93×` faster GPU time!).
  - **`cache_sharpen=on` directly solved bilinear reprojection blur**: on `fast_pan 10%`, PSNR jumped **`31.55 → 33.85 dB` (`+2.30 dB`)**, SSIM **`0.9018 → 0.9424`**, and edge sharpness **`74.9% → 90.9%` (`+16.0 pts`)**; at `15%`, sharpness reached **`101.0%`**.
  - **Empirical Failure Cliff Mapped (`v0.21.0`) & Root-Caused (`v0.22.0`)**: `PAR 5%` originally fell to **`21.63 dB`** (`occlusion_sweep`) and **`20.56 dB`** (`fast_pan`), which `v0.22.0` traced to an `age=0` cache-poisoning bug on deferred unshaded pixels; fixing unshaded provenance (`age=255u`) + adding true `4×4` coarse-to-fine progressive fallback (`coarse_fallback=on`) lifted `fast_pan 5%` to **`32.82 dB` (`+12.26 dB`, beating Hardware VRS Tier 2's `31.78 dB`!)** and `occlusion_sweep 5%` to **`31.08 dB` (`+9.45 dB`)**.
  - **Closed-Loop Auto-Budgeter Proven Under `500 Hz` Stress Shock (`1,188 µs` scene target)**: under `stress 0 → 64 → 0` on `fast_pan`, `AutoTileBudgetController` automatically throttled `tile_budget` from **`100% → 11% → 5%`** (keeping `scene_gpu_us` p95 strictly under budget at `1,064–1,105 µs < 1,188 µs` with `92–100%` critical coverage) and recovered cleanly back to **`100%`** (`185 µs` median) when load cleared.
- **Hard 100% Camera Cut Coarse-to-Fine Recovery & `1080p` Resolution Scaling (`v0.22.0`)**:
  - **100% Hard Camera Cut (`camera_cut` `t = 1.63s`, zero-paused-frame bursts @ 240 Hz)**: without coarse fallback, `PAR 15%` suffered an 85% void on `Cut+0` (`15.18 dB`, `P99=99`); enabling `4×4` coarse-to-fine fallback lifted `Cut+0` by **`+13.77 dB` to `28.95 dB` (`95.4%` sharpness, `499 µs`)**, reached **`34.32 dB` by `Cut+4` (`16.7 ms`)**, and converged to **`39.18 dB` / `0.9865` SSIM (`P99=6`) by `Cut+6` (`25.0 ms`)** at locked `~500 µs` GPU cost. At `5%` budget, camera-cut refinement stays slow (`28.54 → 30.55 dB` over 8 frames, needing ~20 frames to converge).
  - **Resolution Scaling Envelope (`occlusion_sweep`, `120 Hz`, `720p → 1080p`)**: at `1080p` (`1920×1080`), `PAR 15%` ran **`7.25×` faster than Native** (`1,940 µs` vs `14,056 µs`) and **`2.77×` faster than VRS Tier 2** (`5,370 µs`) at `stress=64` with `0` missed deadlines, and **`5.18×` faster than Native** at `stress=32` (`1,466 µs` vs `7,600 µs`). Honest crossover boundary found at `stress=0`: on ultra-light shaders at `1080p`, PAR's fixed visibility/dispatch floor (`1,113 µs`) is **`2.94×` slower** than Native (`379 µs`), proving PAR must bypass when shading cost is negligible.
- **Decoupled Offscreen Upscaling Research & True `1440p`/`4K` Scaling (`v0.23.0`)**:
  - **Spatial Upscaling vs. Progressive Raster (`PAR 15%`) @ `1080p` Output (`Frame #98` @ 240 Hz, `stress=32`)**: `PAR 15%` at full native `1080p` (`905–970 µs`, **`37.42–37.91 dB`**, `0.9769–0.9810` SSIM, `101–103%` sharp, `P99=15–19`) decisively beat `50%` (`960×540`) Catmull-Rom upscaling (`1,534–1,607 µs`, `33.94–34.26 dB`, **`+3.65 dB` better AND `1.66×` faster**), `67%` (`1286×724`) Catmull-Rom upscaling (`2,815–2,875 µs`, `35.62–35.83 dB`, **`+2.29 dB` better AND `2.96×` faster**), and VRS Tier 2 (`2,012–2,079 µs`, `33.43–34.53 dB`). Stacking `67%` offscreen + `PAR 15%` achieved **`15.3×` speedup** (`6,938 → 453 µs`) at `33.83 dB`. `CAS` (`-0.185` weight) oversharpened edges to `130–137%` (`-1.0 dB` vs `catmull_rom`).
  - **True Offscreen `1440p` (`2560×1440`) & `4K` (`3840×2160`) Scaling (`occlusion_sweep`, `120 Hz`)**: at `1440p` `stress=64`, PAR `15%` cut `22,833 µs → 4,584 µs` (**`4.98×` faster**, under `8.33 ms` `120 Hz`); at true **`4K` (`3840×2160`)**, `stress=32` PAR `15%` hit **`6,649 µs` (`4.10×` faster than `27,287 µs` Native, only mode under `120 Hz`)**, and `stress=64` cut **`54,031 µs → 11,215 µs` (`4.82×` faster, `0` misses)**. Fixed `stress=0` visibility floor at `1440p`/`4K` was `1.48–1.50×` Native (`990 µs` / `2,402 µs`).

## Sources

- [NVIDIA Streamline repository](https://github.com/NVIDIA-RTX/Streamline): framework, binaries and build options.
- [NVIDIA DLSS Frame Generation programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md): buffers, support checks, Reflex, presentation ownership and pacing.
- [Intel PresentMon](https://github.com/GameTechDev/PresentMon): Windows frame timing and measurement limitations.
- [AMD FidelityFX Super Resolution 3.1.4](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/): another frame-generation integration reference; verify platform support separately.
- [Microsoft Direct3D 12 synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization): fence-based GPU resource coordination.
- [Decoupled Sampling for Graphics Pipelines (2011)](https://research.nvidia.com/publication/decoupled-sampling-graphics-pipelines): independent visibility and shading sampling.
- [Temporal Coherence Methods in Real-Time Rendering (2012)](https://doi.org/10.1111/j.1467-8659.2012.03075.x): comprehensive temporal reuse techniques.
- [The Visibility Buffer (2013)](https://jcgt.org/published/0002/02/04/): bandwidth-efficient deferred shading architecture.
- [Adaptive Texture Space Shading (2014)](https://onlinelibrary.wiley.com/doi/10.1111/cgf.12303): adaptive surface-space shading.
- [Advanced API Performance: Variable Rate Shading (2022)](https://developer.nvidia.com/blog/?p=36325): hardware VRS optimization considerations.
- [FastAtlas: Real-Time Compact Atlases for Texture Space Shading (2025)](https://doi.org/10.1111/cgf.70010): real-time GPU-accelerated compact texture-space shading caches.
- [Amulet: Frame Extrapolation Through Sparse Layered Scene Representation and Adaptive Shading (2026)](https://arxiv.org/abs/2608.10423): sparse tiled multilayer scene cache, predictive tile lifetimes, and non-neural high-frequency frame extrapolation.
- [Temporally Dense Ray Tracing (NVIDIA, 2019)](https://research.nvidia.com/labs/rtr/publication/andersson2019temporally/): frameless interleaved pixel updates and temporal accumulation for 240+ FPS.
- [STeFAS: Temporally Stable Content-Adaptive and Spatio-Temporal Shading Rate Assignment (TU Wien, 2021)](https://www.cg.tuwien.ac.at/research/publications/2021/stappen_SteFAS/): temporally stable content-driven hardware VRS assignment.
- [Adaptive Display Algorithm (Funkhouser & Séquin, 1993)](https://gfx.cs.princeton.edu/gfx/pubs/Funkhouser_1993_ADA/index.php): constrained image-quality optimization within fixed rendering deadlines.
- [Frameless Rendering (Bishop et al., 1994)](https://doi.org/10.1145/192161.192195): continuous pixel updates from recent input without whole-frame barriers.
- [Interactive Rendering Using the Render Cache (Walter et al., 1999)](https://doi.org/10.2312/EGWR/EGWR99/019-030) and [Enhancing and Optimizing the Render Cache (2002)](https://doi.org/10.2312/EGWR/EGWR02/037-042): reprojected sample caches and prioritized resampling.
- [Adaptive Frameless Rendering (Dayal et al., 2005)](https://doi.org/10.1145/1198555.1198763): closed-loop error-, edge-, and motion-driven sampling.
- [Accelerating Real-Time Shading with Reverse Reprojection Caching (Nehab et al., 2007)](https://pixl.cs.princeton.edu/pubs/Nehab_2007_ARS/index.php) and [Automated Reprojection-Based Pixel Shader Optimization (2008)](https://researchportal.hkust.edu.hk/en/publications/automated-reprojection-based-pixel-shader-optimization/): cross-frame shader result caching.
- [Image-Space Bidirectional Scene Reprojection (Yang et al., 2010)](https://hhoppe.com/proj/bireproj/): depth- and motion-based intermediate frame reconstruction.
- [Coarse Pixel Shading (2014)](https://fileadmin.cs.lth.se/graphics/research/papers/2014/cps/): multi-rate shading within a single shader pipeline.
- [Deferred Adaptive Compute Shading (Mallett & Yuksel, 2018)](https://graphics.geometrian.com/research/dacs.html): variance-driven hierarchical sparse compute shading beyond fixed VRS blocks.
- [Shading Atlas Streaming (Mueller et al., 2018)](https://doi.org/10.1145/3272127.3275087) and [SnakeBinning (2021)](https://doi.org/10.1111/cgf.142648): object-space shading streaming and temporally coherent atlas packing.
- [Time-Warped Foveated Rendering for Virtual Reality Headsets (2020)](https://doi.org/10.1111/cgf.14176): confidence-driven selective redraw maps and geometry culling.
- [Post-Render Warp with Late Input Sampling (2020)](https://doi.org/10.1145/3406187): late camera correction under high latency.
- [A Perceptual Model of Motion Quality (2020)](https://doi.org/10.1145/3386569.3392411), [FovVideoVDP (2021)](https://www.siggraph.org/wp-content/uploads/2021/08/tog-404.html), and [Perceptual Model for Adaptive Local Shading and Refresh Rate](https://www.repository.cam.ac.uk/items/8001a1ac-fbd7-4568-b7bc-e1491f9f82e9): perceptual error and motion-quality modeling.
- [FAST: Filter-Adapted Spatio-Temporal Sampling (2024)](https://doi.org/10.1145/3651283) and [Filtering After Shading with Stochastic Texture Filtering (NVIDIA, 2024)](https://research.nvidia.com/labs/rtr/publication/pharr2024stochtex/): reconstruction-aware stochastic sampling and filtering.
- [GFFE: G-Buffer Free Frame Extrapolation (Intel, 2024)](https://www.intel.com/content/www/us/en/developer/articles/technical/gffe-g-buffer-free-frame-extrapolation.html) and [PatchEX (2025)](https://doi.org/10.1145/3759247): motion classification, disocclusion handling, and parallel patch-based extrapolation.
- [Multi-Layer Reservoir Splatting for Temporal Reuse under Disocclusion (NVIDIA/Utah, 2026)](https://research.nvidia.com/labs/rtr/publication/hong2026multilayer/): multi-layer ReSTIR sample recycling across disocclusions.
- [Construction and Evaluation of an Ultra Low Latency Frameless Renderer for VR (2016)](https://pubmed.ncbi.nlm.nih.gov/26780798/) and [Rethinking the Render-Display Pipeline to Reduce Latency in Interaction (2026)](https://doi.org/10.1145/3799826.3812437): scanout-synchronized frameless rendering hardware references (WDDM-blocked on standard desktop).
- [Defining and Refining Frameless Rendering (Scher Zagier, UNC 1997)](https://www.cs.unc.edu/techreports/97-008.pdf): formalization of frameless spatial scatter, pixel aging, and prioritization.
- [Detail to Attention: Exploiting Visual Tasks for Selective Rendering (Cater, Chalmers & Ward, EGSR 2003)](https://doi.org/10.2312/EGWR/EGWR03/270-280): task-driven inattentional blindness for selective rendering resource allocation.
- [Temporal Resolution Multiplexing: Exploiting the Limitations of Spatio-Temporal Vision for More Efficient VR Rendering (Denes, Maruszczyk, Ash & Mantiuk, IEEE TVCG 2019)](https://ieeexplore.ieee.org/abstract/document/8643566): TCSF-bounded alternating resolution with high-frequency temporal compensation (`2I - F_i`).
- [Spatiotemporal Blue Noise Masks (Wolfe, Morrical, Akenine-Möller & Ramamoorthi, EGSR 2022)](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks): 3D spatial-and-temporal blue noise volume masks for flicker-free interleaved/stochastic rendering.
- [Continuous Temporal Integration in the Human Visual System (Deodato & Melcher, Journal of Vision 2024)](https://pmc.ncbi.nlm.nih.gov/articles/PMC11622157/): continuous psychophysical measurement of ~100 ms temporal integration window under dynamic stimuli.

## Questions to resolve

- Which resolution and monitor should the first experiment target, and what is that configuration's verified upper refresh bound?
- Are native ARM64 DLSS-G binaries and the required runtime features available on this machine?
- Can a small warp consistently meet display deadlines under GPU saturation, and what image quality does it retain?
- What balance of smoothness, image quality and input latency is acceptable when source FPS is very low?
