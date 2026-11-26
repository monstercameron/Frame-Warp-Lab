# Frame Warp Lab

A Direct3D 12 rendering experiment investigating presentation pacing, reprojection and frame generation.

Read [research.md](research.md) for the research and architecture, [test-scene.md](test-scene.md) for the proposed first experiment, and [spacewarp-references.md](spacewarp-references.md) for related implementations and SDKs. [AGENTS.md](AGENTS.md) records the engineering and measurement rules.

## Current baseline

The initial native application creates a D3D12 device, window and double-buffered swap chain, clears the back buffer, presents with VSync, and handles resizing and orderly GPU synchronization. It does not yet draw the test scene or implement reprojection, telemetry, frame generation, or adaptive pacing. It is not a benchmark.

## Build and run on Windows ARM64

With Visual Studio 2022 C++ Build Tools, the ARM64 compiler, Windows SDK, and CMake installed, run in PowerShell from this folder:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A ARM64
cmake --build build --config Debug
./build/Debug/frame_warp_lab.exe
```

Close the window to exit. Debug builds enable the D3D12 debug layer if available. NVIDIA SDK integration is not included and its ARM64 support has not been established.
