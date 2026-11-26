# Spacewarp and reprojection references

Checked 26 November 2026. These are starting points for investigation, not endorsements or verified results on this machine. VR asynchronous spacewarp normally belongs to a headset runtime/compositor; it is not automatically a general-purpose D3D12 library for a desktop monitor.

## Closest desktop references

- [FrameWarp / ReShade Asynchronous Reprojection](https://github.com/xPoiler/Reshade-Asynchronous-Reprojection) is a GPL-3.0 ReShade add-on whose README describes asynchronous camera reprojection for desktop games, including D3D12, with depth, motion and HUD inputs and optional interaction with game frame generation. It describes its own overlay and warp engine. These are author claims, not results independently validated here. It is a particularly relevant architectural reference, but it is an add-on rather than a documented drop-in library for our own renderer. Review its GPL-3.0 terms before copying or integrating code. Windows ARM64 support and performance on our GPU are unverified.
- [Projector](https://github.com/viitana/projector) is a GPL-3.0, explicitly experimental Vulkan asynchronous reprojection demo with a movable camera and adjustable parameters. Its implementation and scene design are relevant to study, but it is not a D3D12 library and would need porting or an independent implementation. Check the license before reusing its code. It requires Vulkan 1.3 and variable-rate shading according to its README; ARM64 support is unverified.
- [Godot asynchronous reprojection](https://github.com/ogilliland/godot-asynchronous-reprojection) describes a Godot implementation. Its repository has no license identified by GitHub; do not assume permission to reuse its code. It is an engine-specific reference rather than a D3D12 SDK.

## VR-oriented options

- The [OpenXR specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html) defines extension mechanisms, including vendor-specific space-warp facilities. Availability and behavior depend on the runtime and hardware; these facilities target XR composition, not a generic desktop swap chain.
- [Meta Application SpaceWarp Unity template](https://github.com/nukadelic/unity-application-spacewarp) is an example for a Unity/Quest XR workflow, with links to Meta's documentation and samples. It is useful for understanding motion-vector and artifact constraints, but is not a native D3D12 desktop library. GitHub did not identify a repository license; verify terms before reusing code.

## Related but distinct

[NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) integrates features including DLSS Frame Generation and Reflex. DLSS-G interpolates generated frames through its presentation integration; it should not be assumed to be a general-purpose asynchronous camera-spacewarp SDK. Its D3D12 feature availability, ARM64 binaries and runtime support remain to be checked on the target machine.

## Suggested next step

Study FrameWarp's publicly available design and Projector's demo at a high level, including scheduling, synchronization, hole handling and benchmarking. Implement a minimal D3D12 warp in our own scene first, and compare with NVIDIA frame generation as a distinct mode. Decide on third-party code reuse only after reviewing licenses, integration boundaries and ARM64 support. None of these projects has been installed or tested locally.
