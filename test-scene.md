# Test scene design: The Motion Lab

The first scene should be a small, repeatable obstacle course for rendering rather than a large game level. Its purpose is to make the benefits of extra displayed frames visible while exposing situations where generated images become misleading. Each feature is deliberately recognizable and has a measurable role.

## Layout

Imagine a long indoor test hall with a clear sightline and several stations along the camera path. A movable camera can travel through it manually or follow a recorded, deterministic path. The hall uses high-contrast floor markings, numbered wall panels and fine geometric detail so stutter, warping and image degradation are easy to spot. Lighting should be stable in the first baseline.

1. **Camera motion corridor:** straight lines, a checkerboard floor and distant lettering show judder during panning and travel. A slow pan should be favorable to camera reprojection; rapid turns and abrupt stops stress prediction and frame age.
2. **Occlusion gate:** narrow foreground pillars alternately hide and reveal a detailed wall. Sideways motion reveals pixels absent from the source frame. An optional bright mask marks disocclusion holes, while a normal view shows the practical artifact. This station also tests near/far depth differences.
3. **Moving-object crossing:** a patterned ball or vehicle moves across the camera path at several speeds, including a sudden reversal. Camera-only reprojection should show its limitations; motion-vector-assisted techniques can be compared. Reversal tests prediction errors.
4. **Fine-detail and transparency station:** thin rails, fences, text, particles and a transparent pane expose edge breakup, ghosting and missing or ambiguous depth. Start with opaque geometry and add transparency and particles in later increments.
5. **Interactive target:** a clearly visible reticle and a target that responds to an input make responsiveness observable. Show timestamps and event IDs in logs. A counter alone is not a physical input-to-photon measurement.
6. **Stress station:** adjustable, measured GPU workload increases rendering cost without changing the camera path. Include controllable spikes as well as steady load to reveal whether warp and presentation can still meet deadlines when the GPU is busy. Calibrate the load per machine instead of assuming a fixed workload means a fixed frame rate.

## Comparison modes

- **Full-rate reference:** render the same scene and deterministic path at the highest sustainable rate. For offline visual comparisons, capture reference images at the exact timestamps being evaluated; live high-rate output is not automatically ground truth if its timestamps differ.
- **Repeat:** render the scene at a controlled lower rate and present the latest completed frame without synthesis.
- **Camera reprojection:** use the latest completed color/depth and camera pose to warp toward the current view. Show both ordinary output and diagnostic holes.
- **NVIDIA frame generation:** add as a separate mode only after SDK binaries and runtime support have been verified; retain NVIDIA's documented presentation and Reflex integration. It is not interchangeable with our own warp.

Keep resolution, scene, camera path, workload, display mode and capture settings matched as closely as possible. Log any differences. Support side-by-side or synchronized captures for analysis; a split-screen live view can itself change GPU load, so benchmark each mode independently too.

## Controls and scenarios

Provide source-rate caps, camera speed, workload intensity, workload spikes, and a toggle between manual and scripted camera motion. Run slow pans, fast pans, lateral movement behind pillars, a stop-and-reverse, moving-object crossings, and camera cuts. A cut should invalidate old history and use a safe fallback. Test with and without VSync or VRR where available, recording the actual display configuration rather than assuming a requested mode is active.

Render the HUD separately where practical, at presentation cadence; report the HUD's update cadence separately from the 3D scene. Include frame IDs and source age, but provide a clean-view toggle so overlays do not obscure visual defects.

## What success and failure look like

Record full-render FPS, actual displayed intervals, missed deadlines, source-frame age, warp GPU time, and source-to-output frame IDs. Examine input latency with appropriate tooling and, when possible, external input-to-photon measurement. Track holes, ghosting, distortions, object-motion errors and UI stability alongside numbers.

A positive result would be more even displayed motion than repeat mode at the same source workload, with tolerable artifacts and no unacceptable latency increase. A failure is also useful: identify the workload, motion or display condition under which the warp misses deadlines or image quality breaks down. Do not claim a universal refresh-rate guarantee from this scene.
