# Development log

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
