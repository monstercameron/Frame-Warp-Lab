# Experiment plan: pacing, latency, then quality

## Objective and priority

Investigate whether frame generation and asynchronous reprojection **together**, supported by variable rate shading (VRS) and the other techniques below, can deliver correctly paced display updates at the monitor's verified upper refresh bound. Apply the user's priorities in this order:

1. **Display pacing:** aim for one correctly timed, useful display update at every refresh opportunity, with no missed deadlines or unintended repeats.
2. **Input latency:** minimize the time from input to a visible and semantically correct response, without sacrificing the pacing target.
3. **Image quality:** minimize visible degradation while retaining the pacing and latency results.

These are optimization priorities and ambitious hypotheses, not a promise that perfection is achievable. A repeated or stale image may preserve the cadence of display updates but does not count as a newly useful motion update. Present calls, generated frame counts and average FPS alone cannot establish perfect pacing. Report real, generated, warped, repeated and dropped frames separately wherever the measurement method permits.

## One coordinated pipeline

1. **Display timing:** identify the target monitor, resolution and verified upper refresh bound; attempt to activate and verify that mode. Derive deadlines from its exact refresh where possible. If the target or displayed timing cannot be verified, report the primary result as inconclusive rather than silently selecting a lower target.
2. **Input and simulation:** timestamp input and update authoritative game state. Sample the latest appropriate camera input near the warp deadline where feasible, without treating an unsimulated camera update as a full world update. Preserve frame and input correlation IDs.
3. **Full scene:** render color, depth, motion vectors and camera transforms with a consistent source-frame ID. Keep the GPU queue bounded and preserve a completed source frame. Use VRS and resolution/quality adjustments to make room for synthesis only after measuring their effect.
4. **Frame generation:** synthesize intermediate world motion using a supported implementation and valid source inputs. Its cadence, buffering and cost must be measured; interpolation may require waiting for a later source frame.
5. **Late reprojection:** update a suitable image toward the latest camera pose within a measured time budget. Depth, motion, source age, holes, moving objects and camera cuts determine whether the warp is safe. The exact placement of warp relative to frame generation is an integration question, not an assumed capability.
6. **UI and presentation:** composite UI or viewmodels late where the chosen presentation integration supports it. Present according to the selected display timing path, without an unbounded backlog. Record missed deadlines, repeats, source age, fallbacks and observed display intervals.

NVIDIA Streamline/DLSS-G can control its own swap chain, pacing and generated frames. Verify SDK binaries and runtime support on Windows ARM64 and establish a documented supported combination before using a custom warp in that path. If that combination cannot be established, report the specific blocker and investigate another supported frame-generation implementation. Single-technique modes remain controls; they do not replace the combined objective.

## Priority 1: establish and protect pacing

- Build a deterministic Motion Lab scene and CLI-controlled runs. Record monitor identity, resolution, supported and active refresh, VRR/VSync state, driver, GPU, workload and all synthesis settings.
- Establish ordinary rendering and frame repetition controls, then measure frame generation, reprojection and their combined path separately. All modes use the same verified upper-bound target and matched workloads where possible.
- Use displayed-change timing or another validated display observation method. Record the distribution of display intervals, missed refresh opportunities, repeats, drops, source frame age and GPU/CPU timings. Make capture limitations explicit; do not infer physical display timing from `Present` alone.
- Reserve and tune GPU headroom for generation and warp, use bounded frames in flight, and test both steady load and spikes. Trial VRS and dynamic resolution as controls for the budget, with fixed settings before adaptive policies.
- Define provisional thresholds before each run and retain raw data. The ideal is zero missed refreshes and correctly spaced useful updates over a stated observation interval; report exact counts and distributions rather than declaring a short clean run universally perfect. When a deadline cannot be met, present a safe fallback and count it separately.

## Priority 2: reduce input latency without losing pacing

- Correlate input events, simulation updates, source frames, generated/warped frames and observed displays. Report camera response and authoritative game-state response separately.
- Compare early versus late input sampling, bounded render queues, frame-start timing, and supported low-latency mechanisms such as Reflex in the SDK's documented path. Monitor any extra latency introduced by waiting for future frames during interpolation.
- Use software timing for instrumented pipeline segments and external hardware or a high-speed camera for physical input-to-photon validation. Mark unmeasurable latency as unavailable; a low timestamp estimate or smooth camera alone is insufficient.
- Reject a latency optimization from the primary candidate if it violates the previously chosen pacing criteria; record the tradeoff rather than hiding it. Compare distributions and worst cases, not only averages.

## Priority 3: improve image quality within the budget

- Compare output against timestamp- and scene-state-matched reference images. Test disocclusions, fast reversals, moving objects, fine detail, transparency, particles, UI and camera cuts.
- Add depth-aware confidence masks and conservative fallbacks; evaluate motion-vector-assisted object prediction and short temporal history for hole filling. Track their GPU cost, error modes, ghosting and invalid-pixel coverage.
- Test VRS at full-rate and controlled coarse rates or masks where supported. Inspect shading-rate boundaries, fine edges, motion and the effect on generated or warped frames. Compare dynamic resolution at matched pacing and workload.
- Preserve pacing first and the chosen latency criteria second. Rank quality changes using predefined metrics and visual inspection. Report the workload and conditions where the tradeoff stops meeting the criteria.

## Core architectural pillars

- Deadline-based scheduling rather than relying solely on VSync.
- Independent scene-rendering and reprojection queues.
- GPU execution headroom reservation.
- Actual displayed-frame timing through PresentMon.
- Independent camera and world-state update frequencies.

## Prioritized experiment sequence

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
| Selective regional repainting | High (implemented `v0.19.0`) |
| Dynamic resolution | Optional fallback |
| Temporal upscaling | Optional |

## Perceptually Amortized Rendering (PAR) research items

Track the following frameless/perceptual rendering items as explicit, independently verifiable experiments building on `v0.19.0`'s visibility-guided selective tile pipeline (see `research.md` for full literature notes and caveats):

| ID | Priority | Research Item | Primary Source(s) | Hypothesis / Target Metric |
| :--- | :---: | :--- | :--- | :--- |
| **PAR-1** | P0 | **Priority-bucketed perceptual tile scheduler** (`P_i = E_i · S_i · A_i / C_i`) + `view=priority` heatmap *(implemented `v0.20.0`)* | Dayal et al. (EGSR 2005); Cater et al. (EGSR 2003); Zagier (1997) | Verified on RTX Spark N1X: raised critical disocclusion/moving coverage from `63.1% → 100.0%` (`fast_pan 15%`) and `84.5% → 100.0%` (`occlusion_sweep 10%`) while running `30–132 µs` faster than FIFO. |
| **PAR-2** | P0 | **3D Spatiotemporal Blue Noise (STBN) refresh phasing** *(implemented `v0.20.0`)* | Wolfe et al. (NVIDIA, EGSR 2022) | Verified 16×16 toroidal void-and-cluster phase LUT (`>= 2.0` tile separation, zero diagonal refresh bands). |
| **PAR-3** | P0 | **Hybrid contiguous + `4×4` sub-tile interleaved shading with `3×3` sliding window** | Andersson et al. (NVIDIA, HPG 2019) | Repaint hard disocclusions as contiguous `16×16` tiles while interleaving background shading refreshes at `4×4` sub-tile granularity without EMA-TAA ghosting. |
| **PAR-4** | P1 | **TCSF-clamped Temporal Resolution Multiplexing (TRM) + 5-tap Catmull-Rom cache sharpening** *(implemented `v0.21.0`)* | Denes, Maruszczyk, Ash & Mantiuk (IEEE TVCG 2019) | Verified on RTX Spark N1X: lifted `fast_pan 10%` PSNR `+2.30 dB` (`31.55 → 33.85 dB`) and edge sharpness `74.9% → 90.9%`; reached `101.0%` sharpness parity at `15%`. |
| **PAR-5** | P0 | **Authoritative vs. synthesized history firewall + foreground/background repair** | Dixit & Sarangi (`PatchEX`, ACM TOG 2025) | Strictly prevent DLSS-G / warped display outputs from feeding back into the persistent scene cache, avoiding recursive geometry, motion, and lighting drift. |
| **PAR-6** | P0 | **Matched-reference PAR quality & perceptual test matrix (Modes A–F)** *(automated PSNR/SSIM/Sharpness harness implemented `v0.21.0`)* | Deodato & Melcher (2024); FovVideoVDP (2021) | Verified on exact `t = 408,366 µs` Frame `#98`: `15%` PAR+SHARP beat VRS Tier 2 by `+4.08 dB` (`occlusion_sweep`) and `+3.80 dB` (`fast_pan`) at `1.9–2.8×` faster GPU time (`v0.22.0` fixed the `age=0` poisoning bug lifting `5%` by `+12.26 dB`, and `v0.23.0` beat `50%/67%` spatial upscaling at `1080p`). Blinded human + FovVideoVDP viewing remains next. |

## Recommended experiments before complex shaders

Execute and gate the following 10 pre-complex-shader validation experiments before investing in full triangle-mesh / complex PBR shader scenes:

| Priority | Experiment | Passing criterion | Current status (`v0.25.0`) |
| :---: | :--- | :--- | :--- |
| 1 | **PAR-3 sub-tile interleaving** | Better temporal quality at equivalent painting cost | Proven (`v0.25.0`: 4-quadrant 100%-SIMD interleaved `4×4` background refresh beat `16×16` blocks across all 4 30-frame sequences by `+0.08–0.22 dB`, higher SSIM, lower `tRMSE` flicker at matched GPU cost; `29%` less flicker than VRS Tier 2) |
| 2 | **PAR-5 history firewall** | No unbounded recursive degradation | Proven (`v0.25.0`: 500-frame `fast_pan` audit showed non-negative drift slope `+0.13 dB/100f` standalone PAR-3 and `+0.37 dB/100f` with `reproject /4`; fg/bg 2-layer LDI remains optional upgrade) |
| 3 | **Damage-rate-aware scheduler** | No sustained repair backlog under feasible load | Proven (`v0.25.0`: damage-rate floor + 4-frame camera-cut burst + `stress=0` auto-bypass dropped `1080p` idle cost `1,113 → 437 µs` and held `100.0%` critical coverage / `0` misses under `stress=64`) |
| 4 | **Independent update frequencies** | Stable output with reduced visibility and shading frequencies | Partial (camera/HUD vs. scene decoupled via `reproject`/`combined` divider; decoupled visibility vs. shading vs. lighting rates pending) |
| 5 | **Multi-resolution scaling** | Predictable performance at 1080p, 1440p and 4K | Proven (`v0.23.0`–`v0.24.0` zero-allocation per-frame DRS up/down + offscreen `1080p` `7.25×`, `1440p` `4.98×`, `4K` `4.82×` PAR / `12.24×` stacked `4K` DRS+PAR with `0` reallocations & live PAR cache survival) |
| 6 | **Temporal video quality harness** | Quality measured across entire motion sequences | Proven (`v0.25.0`: zero-paused-contamination sequence stepping + automated multi-frame PSNR/SSIM/P99/`tRMSE` flicker/drift harness in `scripts/run_temporal_and_firewall_suite.ps1`; optional offline `pyfvvdp` remains bonus) |
| 7 | **Physical display measurements** | Verified displayed cadence and input latency | Planned (requires interactive `WinSta0\Default` run with FrameView / PresentMon `MsBetweenDisplayChange`, `MsUntilDisplayed`, `MsPCLatency`) |
| 8 | **Foreground DLSS-G validation** | Actual generated frames with acceptable quality | Partial (Streamline ARM64 `6×` MFG + Clean-Feed PAR verified with `api_error_count=0`, `0.000%` holes; interactive focused `numFramesActuallyPresented >= 2` + quality check pending) |
| 9 | **Post-synthesis painting** | Measurable improvement over uncorrected synthesis | Planned (repairing residual holes/disocclusions after warp/synthesis rather than only pre-synthesis) |
| 10 | **Blinded perceptual A/B tests** | Establish perceptual equivalence thresholds | Planned (static gaze + smooth-pursuit human viewing against native high-Hz reference) |

## Pre-PBR 8-shader theory stress matrix (`v0.26.0`)

Before building full PBR lighting & textures, isolate each shader failure mode individually against `1×1` Native Ground Truth and Hardware VRS Tier 2 (`scripts/run_shader_theory_suite.ps1`):

| # | Shader (`shader_feature`) | Theory tested | Naive `PAR-3 15%` vs VRS Tier 2 (`v0.26.0` measured) | Architectural takeaway |
| :-: | :--- | :--- | :--- | :--- |
| 1 | **High-frequency checkerboard (`hf_checker`)** | Temporal resampling and detail preservation | `23.05 dB` (`0.872` SSIM) vs VRS `18.28 dB` (`0.669`) | Beats VRS `+4.77 dB`, but un-mipped Nyquist patterns still alias; needs mip/analytical filtering or surface cache. |
| 2 | **Animated texture coordinates (`anim_uv`)** | Material invalidation | **`26.89 dB` (`tRMSE=15.61`)** vs VRS **`33.05 dB` (`8.07`)** | **Exposed failure (`-6.16 dB`)**: animated UV materials MUST emit a reactive invalidation mask. |
| 3 | **Blinn-Phong specular (`specular`)** | View-dependent shading invalidation | `36.59 dB` (`0.975` SSIM) vs VRS `32.58 dB` (`0.945`) | Holds up `+4.01 dB` on corridor angles; sharp curved PBR speculars still warrant split diffuse/specular evaluation. |
| 4 | **Moving point light (`moving_light`)** | Lighting-change propagation | `34.63 dB` (`tRMSE=6.42`) vs VRS `31.39 dB` (`8.21`) | Beats VRS `+3.24 dB`, `-1.96 dB` lag vs baseline; needs light-delta priority boost. |
| 5 | **Animated emissive material (`emissive`)** | Temporal contrast sensitivity | `35.37 dB` (`tRMSE=5.87`) vs VRS `32.31 dB` (`7.61`) | Beats VRS `+3.06 dB`, `-1.22 dB` lag on `10 rad/s` pulsing strips; needs emissive reactive mask. |
| 6 | **Hard moving shadows (`hard_shadows`)** | Nonlocal shading invalidation | `34.02 dB` (**`tRMSE=7.05`**, `P99=30.4`) vs VRS `33.32 dB` (`6.91`) | **Exposed nonlocal lag**: moving shadow edges sweep static walls with zero depth change; needs shadow-delta invalidation. |
| 7 | **Normal mapping (`normal_map`)** | High-frequency surface shading stability | `36.16 dB` (`0.975` SSIM) vs VRS `32.38 dB` (`0.936`) | Rock-solid (`+3.78 dB` over VRS, only `-0.43 dB` vs baseline). |
| 8 | **Simple transparent surfaces (`transparent`)** | Multilayer visibility and reprojection limitations | `32.61 dB` (**`P99=35.8`**, `tRMSE=7.54`) vs VRS `32.58 dB` (`7.44`) | **Exposed multilayer conflict (`-3.98 dB`)**: glass reprojects at background depth; transparency MUST composite in a separate forward pass after opaque PAR. |
| All | **All 8 combined (`all`)** | Combined shader stress | **`15.54 dB` (`0.436` SSIM, `tRMSE=59.95`)** vs VRS `18.27 dB` (`0.686`) | **Naive geometry-only PAR fails when stacked** — motivates shader-aware reactive invalidation + separate forward transparency pass. |

## Industry-leading PBR + anisotropic texturing validation (`v0.27.0`)

Implemented full Filament/Frostbite/OpenPBR-grade PBR (`shaders/pbr_materials.hlsl`, `src/pbr_assets.cpp`) with 6 `256×256` 9-mip `Texture2DArray` PBR materials (sRGB Albedo, unit-renormalized Normal, Toksvig-filtered ORM+Clearcoat), `128×128` FP16 Split-Sum DFG LUT, `64×64×6×6` FP16 GGX pre-filtered HDR Environment Cubemap, Kulla-Conty multiscattering, Burley diffuse, Filament clearcoat, ACES Filmic, and compute-shader analytical `SampleGrad` 16× anisotropic filtering + Kaplanyan-Hill specular AA (`scripts/run_pbr_benchmark.ps1`):

| Configuration (`1280×720`, `stress=0`, `F24..F112` @ 240 Hz) | Scene GPU | PSNR (min) | SSIM | Temporal Flicker (`tRMSE`) | P99 Error |
|---|---:|---:|---:|---:|---:|
| **GT `1×1` Native PBR (`aniso_mip=on`)** | `314 µs` | Reference | `1.0000` | `0.00` | `0.0` |
| **VRS Tier 2 Foveated (`aniso_mip=on`)** | `132 µs` | `25.85 dB` (`25.76`) | `0.8149` | `16.02` | `59.5` |
| **`PAR-3 15%` (`aniso_mip=on`, 16× Aniso `SampleGrad` + SpecAA)** | `254 µs` | **`33.58 dB`** (`32.60`) | **`0.9620`** | **`7.58`** | **`24.3`** |
| **`PAR-3 25%` (`aniso_mip=on`, 16× Aniso `SampleGrad` + SpecAA)** | `268 µs` | **`34.27 dB`** (`32.36`) | **`0.9665`** | **`7.03`** | **`22.4`** |
| **`PAR-3 15%` (`aniso_mip=off`, naive mip-0 control)** | `242 µs` | `30.83 dB` (`29.51`) | `0.9266` | `10.32` | `32.6` |

- **Proven**: On real textured metallic/clearcoat PBR materials, `PAR-3 15%` beats Hardware VRS Tier 2 by **`+7.73 dB`**, `+0.147` SSIM, and `-52.7%` temporal flicker; analytical `SampleGrad` + Toksvig + Specular AA lifts `PAR-3 15%` by **`+2.75 dB`** and cuts flicker `-26.5%` for `+12 µs`.
- **Exposed visibility bottleneck (`stress=0` at `4K`)**: Pure PBR `GT=2,859 µs` vs `PAR-3 15%=2,675 µs` (`1.07×`) because 88-step SDF raymarching runs on 100% of pixels during visibility classification (`~2.2 ms` at 4K). Replacing SDF raymarching with hardware triangle visibility-buffer rasterization is the clear next step to realize `PAR-3`'s full 4–6× speedup on standard PBR workloads.

## Experiment progression and CLI

1. Build agent-accessible, structured logging and CLI inspection/control alongside the baseline. Expose supported and unavailable capabilities, display target, configuration, run lifecycle, per-frame correlation and bounded trace capture. See `AGENTS.md` for the full interface requirements.
2. Implement the deterministic scene, repeat and native-render controls, monitor detection and displayed-timing measurement. Validate mode selection and make failures observable.
3. Implement and validate reprojection, then a supported frame-generation path. Establish the combined integration and its ordering before treating it as the primary experimental candidate.
4. Optimize pacing first: test GPU headroom, queue limits, scheduling and VRS with fixed, reproducible configurations. Run latency and quality measurements during this phase to reveal tradeoffs even though pacing has priority.
5. Hold the pacing criteria constant while optimizing input timing and latency. Then hold both criteria constant while improving image quality.
6. Map the quality envelope across workload, source rate and motion at the monitor upper bound. Retain exact configuration, build version, measurement method, raw traces, criteria and results for each run.

Define concrete tolerances after measuring the system and before evaluating an experiment. A phase may reveal that its goal is infeasible on a given configuration; record that result and the constraints instead of silently lowering the monitor target or concealing degraded frames.
