# Frame Warp Lab

A Direct3D 12 rendering experiment investigating presentation pacing, reprojection and frame generation.

Read [experiment-plan.md](experiment-plan.md) for the prioritized experiment, [research.md](research.md) for the research and architecture, [test-scene.md](test-scene.md) for the proposed test scene, and [spacewarp-references.md](spacewarp-references.md) for related implementations and SDKs. [AGENTS.md](AGENTS.md) records the engineering and measurement rules.

## Current baseline

The native application renders a CPU-driven procedural Motion Lab scene with selectable deterministic camera paths (`corridor_sway`, `fast_pan`, `occlusion_sweep`, `camera_cut`), occluding pillars, moving ball, fine rail, controllable synthetic shading stress, and offscreen linear color, linear view-space depth and 2D screen-space motion-vector targets. A second pass composites tone-mapped output (or depth/motion diagnostic views) and a toggleable late reticle onto a double-buffered D3D12 flip-discard swap chain with VSync, GPU timestamp queries, resize handling and explicit fence synchronization. It does not yet provide repeat/reprojection presentation modes, displayed-frame scanout verification, NVIDIA frame generation, VRS rendering, or adaptive pacing. It is not a benchmark. See [DEVLOG.md](DEVLOG.md) for development history.

## Build and run on Windows ARM64

With Visual Studio 2022 C++ Build Tools, the ARM64 compiler, Windows SDK, and CMake installed, run in PowerShell from this folder:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A ARM64
cmake --build build --config Debug
./build/Debug/frame_warp_lab.exe
```

Close the window to exit. Debug builds enable the D3D12 debug layer if available. NVIDIA SDK integration is not included and its ARM64 support has not been established.

## CLI foundation

The console executable emits JSON and uses exit code 0 for success and 1 for errors. Start the application separately, then use:

```powershell
./build/Debug/frame_warp_cli.exe help
./build/Debug/frame_warp_cli.exe start
./build/Debug/frame_warp_cli.exe instances
./build/Debug/frame_warp_cli.exe status <pid>
./build/Debug/frame_warp_cli.exe events <pid>
./build/Debug/frame_warp_cli.exe trace <pid> [limit]
./build/Debug/frame_warp_cli.exe set <pid> <key> <value>
./build/Debug/frame_warp_cli.exe pause <pid>
./build/Debug/frame_warp_cli.exe resume <pid>
./build/Debug/frame_warp_cli.exe reset <pid>
./build/Debug/frame_warp_cli.exe capabilities
./build/Debug/frame_warp_cli.exe stop <pid>
```

`start` launches the executable next to the CLI and returns its process ID; `ready: false` means the command does not wait for initialization. `instances` discovers application windows, including hidden ones. `status` reads local shared state for a specific process and reports completed present calls, resize count, client size and nominal Windows display modes. These are not displayed-frame measurements. Windows' enumerated highest nominal refresh for the active resolution is not a verified monitor upper bound, and actual displayed timing is unavailable. `stop` requests a normal window close and does not wait for exit. `capabilities` queries a newly created default D3D12 device, not necessarily the running instance's adapter, reporting adapter identity, DXGI tearing support and VRS tier/tile size; VRS support does not mean VRS rendering is implemented.

`events` reads a bounded in-memory journal of the 64 most recent runtime lifecycle, resize and scene-control events, with UTC timestamps and sequence numbers. Events are available only while the instance's shared state exists; the journal is not persisted, and detailed frame traces and error events are not yet implemented. A concurrent overwrite can cause a record to be omitted from a snapshot. Shared state uses a local Windows named mapping and the process's default security descriptor; it is intended for local inspection, not a remote interface.

`trace` reads up to the 256 most recent per-frame telemetry records from the bounded in-memory ring buffer and reports min/median/p95/max summaries in microseconds for CPU Present return interval, CPU source age, and D3D12 GPU timestamp durations (`scene_gpu_us` and `present_gpu_us`). `set` updates runtime parameters asynchronously: `view` (`final`, `depth`, `motion`), `path` (`corridor_sway`, `fast_pan`, `occlusion_sweep`, `camera_cut`), `stress` (`0..64`), and `overlay` (`on`, `off`).

The CLI does not yet manage persistent capture files, measure physical input-to-photon latency, or control reprojection, frame generation or VRS.

`status` reports the Motion Lab timeline and pause state. `pause`, `resume` and `reset` queue scene-control requests; inspect `status` to confirm they were applied. Reset returns the timeline to zero and preserves the pause state. The scripted camera and ball movement are functions of scene time; real-time execution and frame sampling are not yet a deterministic capture or benchmark system.
