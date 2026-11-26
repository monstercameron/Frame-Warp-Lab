# Development log

## v0.4.0 — Initial Motion Lab scene

- Added a procedural D3D12/HLSL scene with a moving camera, checkerboard corridor, alternating foreground pillars, moving patterned ball, fine rail and gridded end wall. Added CLI pause, resume and reset controls and scene-time inspection.
- The first runtime shader compilation failed because `point` and then `line` are reserved HLSL tokens. Renaming them and compiling the pixel shader with the Windows SDK shader compiler exposed and resolved the errors. Added a build dependency so shader edits are copied to the executable directory.
- Verified an ARM64 Debug build and a live run: status reported scene time, pause held the timeline, reset returned it to zero while paused, resume advanced it, and the CLI shut down the process cleanly. This checks process and control behavior; it does not establish display pacing, input latency or image quality.
- This is an initial procedural scene, not the complete planned Motion Lab. User-controlled movement, explicit scene geometry/depth and motion-vector buffers, telemetry captures, frame generation, reprojection, VRS rendering, and actual displayed-frame measurement remain to be built.

## Earlier project history

- v0.1.x established the research plans, prioritized combined frame generation and reprojection at the monitor's upper refresh bound, and added VRS as a research technique.
- v0.2.0 added the runtime inspection CLI foundation; v0.3.0 added launch control and a bounded in-memory event journal. The existing history predates the per-commit log requirement.
