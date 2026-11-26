# Frame Warp Lab

A Direct3D 12 rendering experiment investigating presentation pacing, reprojection and frame generation.

Read [experiment-plan.md](experiment-plan.md) for the prioritized experiment, [research.md](research.md) for the research and architecture, [test-scene.md](test-scene.md) for the proposed test scene, and [spacewarp-references.md](spacewarp-references.md) for related implementations and SDKs. [AGENTS.md](AGENTS.md) records the engineering and measurement rules.

## Current baseline

The native application renders an initial procedural Motion Lab scene with a moving camera, corridor, occluding pillars, moving ball and fine-detail features. It uses a D3D12 window and double-buffered swap chain, presents with VSync, and handles resizing and GPU synchronization. It does not yet provide the full planned scene, explicit scene depth or motion-vector buffers, reprojection, detailed telemetry, frame generation, VRS rendering, or adaptive pacing. It is not a benchmark. See [DEVLOG.md](DEVLOG.md) for development history.

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
./build/Debug/frame_warp_cli.exe pause <pid>
./build/Debug/frame_warp_cli.exe resume <pid>
./build/Debug/frame_warp_cli.exe reset <pid>
./build/Debug/frame_warp_cli.exe capabilities
./build/Debug/frame_warp_cli.exe stop <pid>
```

`start` launches the executable next to the CLI and returns its process ID; `ready: false` means the command does not wait for initialization. `instances` discovers application windows, including hidden ones. `status` reads local shared state for a specific process and reports completed present calls, resize count, client size and nominal Windows display modes. These are not displayed-frame measurements. Windows' enumerated highest nominal refresh for the active resolution is not a verified monitor upper bound, and actual displayed timing is unavailable. `stop` requests a normal window close and does not wait for exit. `capabilities` queries a newly created default D3D12 device, not necessarily the running instance's adapter, reporting adapter identity, DXGI tearing support and VRS tier/tile size; VRS support does not mean VRS rendering is implemented.

`events` reads a bounded in-memory journal of the 64 most recent runtime lifecycle, resize and scene-control events, with UTC timestamps and sequence numbers. Events are available only while the instance's shared state exists; the journal is not persisted, and detailed frame traces and error events are not yet implemented. A concurrent overwrite can cause a record to be omitted from a snapshot. Shared state uses a local Windows named mapping and the process's default security descriptor; it is intended for local inspection, not a remote interface.

The CLI does not yet collect detailed traces, manage scenes or experiment configurations, measure latency, or control generation, reprojection or VRS. Persistent structured logs and the remaining agent operations are still planned.

`status` reports the Motion Lab timeline and pause state. `pause`, `resume` and `reset` queue scene-control requests; inspect `status` to confirm they were applied. Reset returns the timeline to zero and preserves the pause state. The scripted camera and ball movement are functions of scene time; real-time execution and frame sampling are not yet a deterministic capture or benchmark system.
