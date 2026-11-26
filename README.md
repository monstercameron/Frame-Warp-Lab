# Frame Warp Lab

A Direct3D 12 rendering experiment investigating presentation pacing, reprojection and frame generation.

Read [experiment-plan.md](experiment-plan.md) for the prioritized experiment, [research.md](research.md) for the research and architecture, [test-scene.md](test-scene.md) for the proposed test scene, and [spacewarp-references.md](spacewarp-references.md) for related implementations and SDKs. [AGENTS.md](AGENTS.md) records the engineering and measurement rules.

## Current baseline

The native application renders a CPU-driven procedural Motion Lab scene with selectable deterministic camera paths (`corridor_sway`, `fast_pan`, `occlusion_sweep`, `camera_cut`) and low-latency interactive manual controls (`manual`: Win32 Raw Input mouse deltas with left-click drag or right-click/`M` mouse-look, `ESC` unlock, `WASD`/`Q`/`E` move, arrow keys look, `1-5` presentation modes, `B` VRS mode cycle, `K` adaptive deadline scheduler toggle, `G` async dual-queue toggle, `L` pacer mode, `H` holes view, `O` HUD toggle, `F` Reflex optical flash box toggle, `V` VSync toggle, `C` cycle paths, `[`/`]` divider, `-`/`=` stress, `P`/`Space` pause, `R` reset). The loop waits on the waitable swap chain and GPU fence before sampling input (`wait_before_input_sample: true`), paces `Present()` entry with a DWM phase-locked high-resolution deadline pacer, executes scene rendering (with optional D3D12 Variable Rate Shading Tier 1 `uniform_2x2` and Tier 2 `foveated` screen-space shading-rate image) and late synthesis/presentation on independent D3D12 command queues (`NORMAL` scene queue + `HIGH`-priority present queue backed by a 3-slot source history ring), optionally adapts rendering/VRS/synthesis tiers automatically via a deadline-aware GPU headroom scheduler, and emits NVIDIA PC Latency (PCL) ETW TraceLogging markers (`PCLStatsTraceLoggingProvider`, including late-warp markers) for FrameView, NVIDIA App, and PresentMon. It outputs offscreen linear color, linear view-space depth and 2D screen-space motion-vector targets, and composites a late 1x1-shaded 2D HUD layer with a 128-frame color-coded frametime graph, smoothed Present-cadence FPS/frametime readout, GPU pass timings, input-to-present latency, and center reticle. The presentation pass supports `full_render`, `repeat`, depth-based late camera `reproject`, motion-vector extrapolation `frame_gen`, and `combined` (late camera warp + independent moving-object extrapolation + depth-gated `T_{n-1}` temporal history disocclusion recovery) on a waitable, tearing-capable D3D12 flip-discard swap chain with per-queue D3D12 GPU timestamp queries. It does not yet provide verified physical displayed-frame scanout timing (which requires elevated PresentMon/FrameView capture) or NVIDIA Streamline/DLSS-G integration. It is not a benchmark. See [DEVLOG.md](DEVLOG.md) for development history.

## Build and run on Windows ARM64

With Visual Studio 2022 C++ Build Tools, the ARM64 compiler, Windows SDK, and CMake installed, run in PowerShell from this folder:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A ARM64
cmake --build build --config Release
./build/Release/frame_warp_lab.exe
```

Use `--config Debug` to enable the D3D12 debug layer if available, or `--config Release` for low-overhead ARM64 NEON SIMD pacing measurements. Close the window to exit. NVIDIA Streamline/DLSS-G integration is not yet included (`v2.14.16` publishes `aarch64`/`arm64ec` releases that remain to be evaluated locally).

## CLI foundation

The console executable emits JSON and uses exit code 0 for success and 1 for errors. Start the application separately, then use:

```powershell
./build/Debug/frame_warp_cli.exe help
./build/Debug/frame_warp_cli.exe start
./build/Debug/frame_warp_cli.exe instances
./build/Debug/frame_warp_cli.exe status <pid>
./build/Debug/frame_warp_cli.exe events <pid>
./build/Debug/frame_warp_cli.exe trace <pid> [limit] [summary]
./build/Debug/frame_warp_cli.exe set <pid> <key> <value>
./build/Debug/frame_warp_cli.exe pause <pid>
./build/Debug/frame_warp_cli.exe resume <pid>
./build/Debug/frame_warp_cli.exe reset <pid>
./build/Debug/frame_warp_cli.exe capabilities
./build/Debug/frame_warp_cli.exe stop <pid>
```

`start` launches the executable next to the CLI and returns its process ID; `ready: false` means the command does not wait for initialization. `instances` discovers application windows, including hidden ones. `status` reads local shared state for a specific process and reports completed present calls, resize count, client size and nominal Windows display modes. These are not displayed-frame measurements. Windows' enumerated highest nominal refresh for the active resolution is not a verified monitor upper bound, and actual displayed timing is unavailable. `stop` requests a normal window close and does not wait for exit. `capabilities` queries a newly created default D3D12 device, not necessarily the running instance's adapter, reporting adapter identity, DXGI tearing support and VRS tier/tile size; VRS support does not mean VRS rendering is implemented.

`events` reads a bounded in-memory journal of the 64 most recent runtime lifecycle, resize and scene-control events, with UTC timestamps and sequence numbers. Events are available only while the instance's shared state exists; the journal is not persisted, and detailed frame traces and error events are not yet implemented. A concurrent overwrite can cause a record to be omitted from a snapshot. Shared state uses a local Windows named mapping and the process's default security descriptor; it is intended for local inspection, not a remote interface.

`trace` reads up to the 256 most recent per-frame telemetry records from the bounded in-memory ring buffer (pass `summary` as the fifth argument to omit individual sample records) and reports frame-kind counts (`rendered`, `repeated`, `warped`, `generated`, `combined`, `fallback`), `async_overlaps`, `missed_deadlines`, and min/median/p95/max summaries for CPU `Present()` entry interval, CPU source age, input-to-present latency, CPU stage timings (`sim_cpu_us`, `render_submit_cpu_us`, `present_call_cpu_us`), D3D12 GPU timestamp durations (`scene_gpu_us` and `present_gpu_us`), and GPU-measured `invalid_pixel_ratio` and `temporal_recovered_ratio`. `set` updates runtime parameters asynchronously: `mode` (`full_render`, `repeat`, `reproject`, `frame_gen`, `combined`), `pacer` (`deadline`, `vsync_only`, `unlocked`), `target_fps` (`0` for exact DWM display refresh or `30..500`), `async_gpu` (`on`, `off`), `vrs` (`off`, `uniform_2x2`, `foveated`), `adaptive` (`on`, `off`), `divider` (`1..8`), `vsync` (`on`, `off`), `view` (`final`, `depth`, `motion`, `holes`, `vrs`), `path` (`corridor_sway`, `fast_pan`, `occlusion_sweep`, `camera_cut`, `manual`), `stress` (`0..64`), `overlay` (`on`, `off`), and `flash` (`on`, `off`).

The CLI does not yet manage persistent capture files, measure physical input-to-photon latency or display scanout timing, or control NVIDIA DLSS-G via Streamline.

`status` reports the Motion Lab timeline and pause state. `pause`, `resume` and `reset` queue scene-control requests; inspect `status` to confirm they were applied. Reset returns the timeline to zero and preserves the pause state. The scripted camera and ball movement are functions of scene time; real-time execution and frame sampling are not yet a deterministic capture or benchmark system.
