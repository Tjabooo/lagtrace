# Changelog

## 0.1.0 (2026-11-10)

First release.

- `VK_LAYER_LAGTRACE_latency`: a Vulkan layer that probes a small region of every presented frame,
  logs kernel click timestamps (evdev) and the driver's first-pixel-out times (`VK_EXT_present_timing`).
  About 13 µs of CPU per frame; the game never waits on the GPU because of it.
- `lagtrace-bench`: a minimal SDL3 + Vulkan test app (present mode, swapchain size, FPS cap, CPU load,
  X11 or Wayland).
- `lagtrace` CLI: `bench`, `suite`, `run`, `attach`, `compare` (95% CI of the median difference,
  Mann-Whitney test), `show`, `list`, `analyze`, `doctor`. Virtual mouse through `/dev/uinput`.
- SVG/PNG charts.
