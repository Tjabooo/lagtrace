# How lagtrace works

lagtrace measures the same thing a photodiode rig (LDAT, OSLTT, an Arduino with a light sensor) does:
the time from a click to the moment the picture changes because of it. The difference is where each
end of the measurement comes from.

| | Light-sensor rig | lagtrace |
|---|---|---|
| Click time | the rig presses the button itself | the kernel's timestamp of the button event (evdev, `CLOCK_MONOTONIC`) |
| "The picture changed" | the sensor sees the pixels light up | the layer reads the pixels the GPU produced |
| "It's on screen" | light reaches the sensor | the driver reports when the frame's first pixel went out to the display |
| Display's own delay | included | **not included** (add your monitor's processing time from a review) |

## The three parts

**`VK_LAYER_LAGTRACE_latency`** is a Vulkan layer (`layer/lagtrace_layer.c`, one C file, no
dependencies). It's implicit: the Vulkan loader puts it into any program started with `LAGTRACE=1`,
including Proton/DXVK/VKD3D games and anything inside gamescope. It does four things:

1. **Watches input devices.** An input thread reads `/dev/input/event*` for key and button presses.
   `EVIOCSMASK` makes the kernel deliver only `EV_KEY` events, so mouse motion never wakes it.
   `EVIOCSCLOCKID` puts the timestamps on `CLOCK_MONOTONIC`, the clock everything else uses. New
   devices (like the CLI's virtual mouse) are picked up with inotify.
2. **Probes every frame.** At each `vkQueuePresentKHR` it records one extra command buffer on the
   presenting queue. That buffer waits on the game's "rendering done" semaphores, copies a small region
   of the swapchain image (64×64 pixels at the centre by default) into host-visible memory, and signals
   a semaphore the present then waits on instead. The swapchain gets `TRANSFER_SRC` usage added at
   creation, if the surface supports it. The game's threads never wait on any of this. A worker thread
   waits on the copy's fence, which signals when the GPU has finished the game's frame (the copy only
   runs after it), and notes that time.
3. **Asks for display timing.** With `VK_EXT_present_timing` (Mesa 26.2+), every frame presented in the
   window after a press carries a timing request for `IMAGE_FIRST_PIXEL_OUT`: on Wayland and in
   gamescope that's the compositor's real flip. XWayland only reports `REQUEST_DEQUEUED`, which is the
   flip when vsync is on.
4. **Writes a log.** Lines go to an in-memory buffer and a separate thread writes them to
   `/dev/shm/lagtrace-<uid>/<pid>.log` every 25 ms, so the game never does file I/O for lagtrace. Frames
   are only logged around presses: the 12 before each press, and every frame up to
   `LAGTRACE_WINDOW_MS` (300) after it.

**`lagtrace-bench`** (`bench/lagtrace_bench.c`, SDL3 + Vulkan) is the smallest possible game. Each
frame it waits for a swapchain image, reads input, then clears the whole image white if the left
button is held and black if not. It has the knobs that matter for latency: present mode, swapchain
image count, a sleep-based FPS cap, simulated CPU work per frame, X11 vs Wayland.

**`lagtrace`** (Python, standard library only) creates the virtual mouse through `/dev/uinput`,
starts the program, clicks with random gaps (so clicks land at every point of the refresh cycle),
then reads the log and does the analysis.

## Turning a log into latencies

For each press:

1. **Which swapchain:** the one with the most frames around the press.
2. **Baseline:** the probe's mean luma in the last frame presented before the press. **Noise:** the
   biggest frame-to-frame change among the (up to 8) frames before it.
3. **Response frame:** the first frame presented after the press whose luma differs from the baseline
   by more than `max(threshold, 4 × noise)`. The default threshold is 8 on a 0-255 scale. Comparing to
   the baseline, not to the previous frame, means the button's release (back to the baseline) is never
   mistaken for the press.
4. **End time:** the display time of the first frame at or after the response frame that reached the
   screen (frames the compositor dropped are skipped). Without display timing, it's when the GPU
   finished the response frame, and lagtrace says so.
5. **Stages:** click → the response frame's present call ("click → present": the game noticing the
   input and building the frame), present call → GPU done ("GPU"), GPU done → first pixel out
   ("compositor + display": waiting in the swapchain queue, composition, waiting for the vblank).

A press is a miss (counted, never guessed) if there's no baseline, nothing changed before the next
press or the end of the window, or no display time came back.

## The log format

All times are `CLOCK_MONOTONIC` nanoseconds.

```
# lagtrace log 1
I <pid> <start> <process name>
O <version> <probe cx> <probe cy> <probe size> <window ms>
G <gpu name>|<driver name> <driver info>
D <device> <input device name>
W <sc> <width> <height> <VkFormat> <VkPresentModeKHR> <timing 0/1> <probe 0/1> <px> <py> <pw> <ph> <images>
C <t> <device> <key code>            press
R <t> <device> <key code>            release
F <sc> <frame> <t>                   vkQueuePresentKHR called
P <sc> <frame> <luma> <t gpu done>   probe read back
S <sc> <frame> <t> px|dq|drop        first pixel out / XWayland dequeued / never shown
Z <sc>                               swapchain destroyed
X <warning ...>
```

`lagtrace analyze <log>` re-runs the analysis on a raw log, and every result keeps its log next to it
as `.log.gz`.

## Accuracy

What's in the number:

- everything from the kernel receiving the click to the frame's first pixel leaving the GPU: the
  compositor's input routing, the game noticing the input, rendering, the swapchain queue, composition
  and the wait for scanout.

What's not:

- **The mouse itself**: switch debounce, its sensor, the USB polling interval (up to 1 ms at 1000 Hz,
  0.125 ms at 8000 Hz).
- **The display**: processing, and the pixel response time. Monitor reviews (RTINGS etc.) measure these.
- **The scanline.** "First pixel out" is when scanout of the frame starts at the top of the screen. A
  pixel halfway down appears about half a refresh later (≈2 ms at 240 Hz). lagtrace reports the frame,
  the same as PresentMon's display latency.

Checks you can repeat:

- **The virtual mouse's timestamp.** The CLI notes when it writes each click to `/dev/uinput`; the
  kernel's timestamp is a few microseconds later (`inject_median_us` in each result).
- **The refresh-cycle test.** An uncapped bench in mailbox mode finishes a frame within microseconds,
  so the answer to a click just waits for the next vblank. Latency should then be spread evenly over
  one refresh interval: median ≈ half a refresh, 5th to 95th percentile ≈ 0.05 to 0.95 of a refresh.
  At 240 Hz (4.17 ms) lagtrace measures a median of 2.2-2.5 ms and a 5th-95th range of 0.4-3.9 ms.
- **Vsync spreads by one refresh.** With FIFO, the 5th-95th range is about one refresh wide too, just
  shifted by the frames waiting in the queue.

A comparison against a photodiode is the obvious next check. If you have one, please share your numbers
in an issue.

## Cost

One small copy and one extra queue submission per frame, plus a lock-protected log append around
presses. The uncapped bench goes from about 17,400 to 14,100 fps with lagtrace on, which is about
13 µs of CPU per frame: 0.4% of a frame at 300 fps. Nothing waits on the GPU.
