# Frame Warp Lab: initial research

Research started 26 November 2026. The aim is to investigate steady, responsive display updates even when a complete scene cannot be rendered at the monitor's refresh rate. This is a research hypothesis, not a validated renderer.

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

### Controlled comparison

Run the deterministic Motion Lab path in four initial modes: full-render baseline, lower-rate frame repetition, our late camera reprojection, and NVIDIA frame generation if supported. Match workload, camera path, resolution and display settings as closely as practical; benchmark modes independently because displaying multiple views can itself change GPU load. Where a high-rate reference is used for image comparisons, match scene state and timestamps. Record differences in configuration and feature availability.

For each run capture: actual displayed intervals and missed refreshes; real scene frame times; source age at display; CPU and GPU timings for render and warp; queue depth; the rate and reasons for fallback; and input-to-display latency where observable. Inspect disocclusion holes, motion errors, ghosting, transparency, UI and camera-cut behavior. A successful result is more even motion than repetition at the same source workload with acceptable image quality and latency, not merely a higher reported FPS. External instrumentation or a high-speed camera is needed for physical input-to-photon validation.

## API decision

On 26 November 2026, we chose **Direct3D 12** for the first renderer. Vulkan remains a possible future backend, but D3D12 is the implementation target for this experiment. Verify the native Windows ARM64 compiler, Windows SDK, graphics tooling and NVIDIA runtime support before relying on any optional integration.

## First experiment

Build a native Windows D3D12 test scene with a movable camera, static geometry, moving objects, foreground occluders and separate UI. First establish ordinary rendering and frame repetition, then add depth-based camera reprojection with a visible invalid-pixel mask. Inject controllable rendering load and test several source rates, resolutions and refresh rates, including overload.

Use deterministic camera paths and repeatable inputs. Test sudden turns, moving objects, camera cuts and newly exposed surfaces. Compare against a high-rate reference at matching timestamps, while recognizing that image error is only a quality proxy. Add DLSS-G as an independently validated comparison if the SDK binaries and runtime support it.

Record separately: full scene FPS and frame-time distribution; actual displayed intervals and missed refreshes; source-frame age; input-to-display latency where measurable; CPU and GPU time for scene, warp and presentation; and artifacts such as holes, ghosting and unstable UI. Record GPU, driver, OS, monitor refresh, resolution, VSync/VRR state and capture method. A software timestamp is not a physical input-to-photon measurement; external measurement or a high-speed camera is needed for that.

PresentMon can capture Windows CPU, GPU and display-related metrics, but documents limitations including GPU timing accuracy with hardware-accelerated GPU scheduling. NVIDIA recommends FrameView's displayed-change metric for DLSS-G pacing because present-call timing alone can be misleading.

## Machine observation

On 26 November 2026, `nvidia-smi` reported an NVIDIA RTX Spark N1X in WDDM mode and driver 621.40; local Python is ARM64. This does not establish DLSS-G support, monitor refresh, usable SDK binaries or achievable timing. Verify those during implementation.

## Sources

- [NVIDIA Streamline repository](https://github.com/NVIDIA-RTX/Streamline): framework, binaries and build options.
- [NVIDIA DLSS Frame Generation programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md): buffers, support checks, Reflex, presentation ownership and pacing.
- [Intel PresentMon](https://github.com/GameTechDev/PresentMon): Windows frame timing and measurement limitations.
- [AMD FidelityFX Super Resolution 3.1.4](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/): another frame-generation integration reference; verify platform support separately.
- [Microsoft Direct3D 12 synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization): fence-based GPU resource coordination.

## Questions to resolve

- Which refresh rates and resolutions should the first experiment target?
- Are native ARM64 DLSS-G binaries and the required runtime features available on this machine?
- Can a small warp consistently meet display deadlines under GPU saturation, and what image quality does it retain?
- What balance of smoothness, image quality and input latency is acceptable when source FPS is very low?
