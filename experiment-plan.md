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
| **PAR-1** | P0 | **Priority-bucketed perceptual tile scheduler** (`P_i = E_i · S_i · A_i / C_i`) + `view=priority` heatmap | Dayal et al. (EGSR 2005); Cater et al. (EGSR 2003); Zagier (1997) | Replace FIFO CAS threadgroup bias with 8-bin GPU priority admission so tight budgets (`5%–20%`) always repaint reticle-area disocclusions, high-gradient edges, and aging tiles first. |
| **PAR-2** | P0 | **3D Spatiotemporal Blue Noise (STBN) refresh phasing** | Wolfe et al. (NVIDIA, EGSR 2022) | Replace modular periodic tile hashing with a `64×64×16` 3D STBN mask to eliminate crawling refresh waves and temporal clumping. |
| **PAR-3** | P0 | **Hybrid contiguous + `4×4` sub-tile interleaved shading with `3×3` sliding window** | Andersson et al. (NVIDIA, HPG 2019) | Repaint hard disocclusions as contiguous `16×16` tiles while interleaving background shading refreshes at `4×4` sub-tile granularity without EMA-TAA ghosting. |
| **PAR-4** | P1 | **TCSF-clamped Temporal Resolution Multiplexing (TRM) detail compensation** | Denes, Maruszczyk, Ash & Mantiuk (IEEE TVCG 2019) | Apply flicker-bounded compensation `I + α(I - F_cached)` on freshly repainted tiles / alternating frames at `120–240+ Hz` to cancel bilinear cache softening without exceeding TCSF flicker limits. |
| **PAR-5** | P0 | **Authoritative vs. synthesized history firewall + foreground/background repair** | Dixit & Sarangi (`PatchEX`, ACM TOG 2025) | Strictly prevent DLSS-G / warped display outputs from feeding back into the persistent scene cache, avoiding recursive geometry, motion, and lighting drift. |
| **PAR-6** | P0 | **Matched-reference PAR quality & perceptual test matrix (Modes A–F)** | Deodato & Melcher (2024); FovVideoVDP (2021) | Compare `240 Hz native`, `60→240 Hz warp`, and `30→240/480 Hz` with `5% / 10% / 20%` priority patch budgets using matched-timestamp PSNR/SSIM/FovVideoVDP and blinded viewing under static gaze vs. smooth pursuit/fast turns. |

## Experiment progression and CLI

1. Build agent-accessible, structured logging and CLI inspection/control alongside the baseline. Expose supported and unavailable capabilities, display target, configuration, run lifecycle, per-frame correlation and bounded trace capture. See `AGENTS.md` for the full interface requirements.
2. Implement the deterministic scene, repeat and native-render controls, monitor detection and displayed-timing measurement. Validate mode selection and make failures observable.
3. Implement and validate reprojection, then a supported frame-generation path. Establish the combined integration and its ordering before treating it as the primary experimental candidate.
4. Optimize pacing first: test GPU headroom, queue limits, scheduling and VRS with fixed, reproducible configurations. Run latency and quality measurements during this phase to reveal tradeoffs even though pacing has priority.
5. Hold the pacing criteria constant while optimizing input timing and latency. Then hold both criteria constant while improving image quality.
6. Map the quality envelope across workload, source rate and motion at the monitor upper bound. Retain exact configuration, build version, measurement method, raw traces, criteria and results for each run.

Define concrete tolerances after measuring the system and before evaluating an experiment. A phase may reveal that its goal is infeasible on a given configuration; record that result and the constraints instead of silently lowering the monitor target or concealing degraded frames.
