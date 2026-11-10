"""Read a lagtrace layer log and turn presses into latency samples.

Log lines (all times are CLOCK_MONOTONIC nanoseconds):
  I pid start comm             O version cx cy size window_ms     G gpu|driver
  D dev name                   input device the layer watches
  C t dev code / R t dev code  press / release (kernel timestamp)
  W sc w h format mode timing probe px py pw ph images             swapchain created
  F sc seq t                   frame handed to vkQueuePresentKHR (logged around presses)
  P sc seq luma t_gpu          probe region's mean luma (0-255); when the GPU finished it
  S sc seq t kind              display time: px = first pixel out, dq = XWayland "complete",
                               drop = the compositor never showed this frame
  Z sc                         swapchain destroyed;   X ...  layer warnings
  E gamescope                  the program runs inside gamescope
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field

from . import stats

PRESENT_MODES = {0: "immediate", 1: "mailbox", 2: "fifo", 3: "fifo-relaxed", 1000361000: "fifo-latest-ready"}
VSYNC_MODES = {2, 3, 1000361000}
FORMATS = {44: "B8G8R8A8_UNORM", 50: "B8G8R8A8_SRGB", 37: "R8G8B8A8_UNORM", 43: "R8G8B8A8_SRGB",
           58: "A2R10G10B10_UNORM", 64: "A2B10G10R10_UNORM", 97: "R16G16B16A16_SFLOAT"}
BTN_LEFT = 0x110
VIRTUAL_MOUSE = "lagtrace virtual mouse"
VIRTUAL_PAD = "lagtrace virtual pad"


@dataclass
class Frame:
    seq: int
    t_present: int
    luma: float | None = None
    t_gpu: int | None = None
    t_shown: int | None = None   # display time, 0 = dropped
    shown_kind: str | None = None


@dataclass
class Swapchain:
    key: int
    width: int
    height: int
    fmt: int
    mode: int
    timing: bool
    probe: bool
    probe_rect: tuple
    images: int
    frames: dict = field(default_factory=dict)

    @property
    def mode_name(self) -> str:
        return PRESENT_MODES.get(self.mode, str(self.mode))

    def frame(self, seq: int, t: int | None = None) -> Frame:
        f = self.frames.get(seq)
        if f is None:
            f = self.frames[seq] = Frame(seq, t or 0)
        elif t is not None:
            f.t_present = t
        return f


@dataclass
class Press:
    t: int
    dev: int
    code: int
    device: str


@dataclass
class Log:
    path: str = ""
    pid: int = 0
    comm: str = ""
    version: str = ""
    probe_opts: tuple = ()
    window_ms: int = 300
    gpu: str = ""
    devices: dict = field(default_factory=dict)
    presses: list = field(default_factory=list)
    releases: list = field(default_factory=list)
    swapchains: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    gamescope: bool = False  # the program ran inside gamescope


def parse(text: str, path: str = "") -> Log:
    log = Log(path=path)
    current = {}  # layer swapchain id -> Swapchain (ids can repeat across devices)
    for line in text.splitlines():
        if not line or line[0] == "#":
            continue
        p = line.split()
        tag = p[0]
        try:
            if tag in ("F", "P", "S"):
                sc = current.get(int(p[1]))
                if sc is None:
                    continue
                seq = int(p[2])
                if tag == "F":
                    sc.frame(seq, int(p[3]))
                elif tag == "P":
                    f = sc.frame(seq)
                    f.luma, f.t_gpu = float(p[3]), int(p[4])
                else:
                    f = sc.frame(seq)
                    f.t_shown, f.shown_kind = int(p[3]), p[4]
            elif tag in ("C", "R"):
                dev = int(p[2])
                pr = Press(int(p[1]), dev, int(p[3]), log.devices.get(dev, "?"))
                (log.presses if tag == "C" else log.releases).append(pr)
            elif tag == "W":
                v = [int(x) for x in p[1:13]]
                sc = Swapchain(len(log.swapchains), v[1], v[2], v[3], v[4], bool(v[5]), bool(v[6]),
                               (v[7], v[8], v[9], v[10]), v[11])
                log.swapchains.append(sc)
                current[v[0]] = sc
            elif tag == "D":
                log.devices[int(p[1])] = line.split(None, 2)[2] if len(p) > 2 else "?"
            elif tag == "I":
                log.pid, log.comm = int(p[1]), " ".join(p[3:])
            elif tag == "O":
                log.version = p[1]
                log.probe_opts = (float(p[2]), float(p[3]), int(p[4]))
                log.window_ms = int(p[5])
            elif tag == "G":
                log.gpu = line[2:].strip()
            elif tag == "E":
                log.gamescope = log.gamescope or (len(p) > 1 and p[1] == "gamescope")
            elif tag == "X":
                log.warnings.append(line[2:].strip())
        except (IndexError, ValueError):
            continue  # a line cut off when the game was killed
    return log


def read(path: str) -> Log:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return parse(fh.read(), path)


def pick_main(logs: list[Log]) -> Log | None:
    """The log of the process whose frames reach the display.

    Run inside a nested gamescope, both the game and gamescope load the layer. The game's
    display times are then gamescope's internal ones; gamescope's own window is what the
    desktop puts on screen, and it shows the game's frames too, so its log measures the
    whole chain. Headless or embedded gamescope has no swapchain: then it's the game's log.
    """
    def frames(lg: Log) -> int:
        return sum(len(s.frames) for s in lg.swapchains)

    def score(lg: Log):
        outer = lg.comm.startswith("gamescope") and any(s.timing and s.probe for s in lg.swapchains)
        return (outer, frames(lg), len(lg.swapchains))
    candidates = [lg for lg in logs if lg.swapchains]
    return max(candidates, key=score) if candidates else (logs[0] if logs else None)


def _press_filter(log: Log, device: str | None, code: int | None, since: int | None):
    out = []
    for pr in log.presses:
        name = log.devices.get(pr.dev, pr.device)
        pr.device = name
        if device and device.lower() not in name.lower():
            continue
        if code is not None and pr.code != code:
            continue
        if since is not None and pr.t < since:
            continue
        out.append(pr)
    return out


def _swapchain_for(log: Log, t: int, window_ns: int):
    best, best_n = None, 0
    for sc in log.swapchains:
        n = sum(1 for f in sc.frames.values() if t - 200_000_000 <= f.t_present <= t + window_ns)
        if n > best_n:
            best, best_n = sc, n
    return best


def _is_xwayland(sc: Swapchain) -> bool:
    """XWayland reports "dequeued" (X Present complete) instead of first pixel out."""
    return any(f.shown_kind == "dq" for f in sc.frames.values())


def _has_display_times(sc: Swapchain) -> bool:
    """Some compositors answer every timing request without a time (gamescope's Wayland
    server, for one). Then the GPU-done time is all there is."""
    return any(f.t_shown for f in sc.frames.values())


def analyze(log: Log, *, device: str | None = None, code: int | None = BTN_LEFT, since: int | None = None,
            threshold: float = 8.0, noise_mult: float = 4.0) -> dict:
    """Latency samples for the presses in a log.

    For each press: the baseline is the probe's luma in the last frame presented before
    it, and the noise is the largest frame-to-frame change among the frames before it.
    The response is the first frame presented after the press whose luma differs from the
    baseline by more than max(threshold, noise * noise_mult). Its end time is when the
    first frame at or after it reached the display.
    """
    window_ns = log.window_ms * 1_000_000
    presses = _press_filter(log, device, code, since)
    samples, misses = [], []
    for i, pr in enumerate(presses):
        nxt = presses[i + 1].t if i + 1 < len(presses) else pr.t + window_ns
        sc = _swapchain_for(log, pr.t, window_ns)
        if sc is None:
            misses.append({"t": pr.t, "reason": "no frames"})
            continue
        frames = sorted((f for f in sc.frames.values() if f.t_present), key=lambda f: f.seq)
        pre = [f for f in frames if f.t_present < pr.t and f.luma is not None][-8:]
        if len(pre) < 2:
            misses.append({"t": pr.t, "reason": "no baseline"})
            continue
        base = pre[-1].luma
        noise = max(abs(a.luma - b.luma) for a, b in zip(pre, pre[1:]))
        thr = max(threshold, noise * noise_mult)
        post = [f for f in frames if pr.t <= f.t_present < min(nxt, pr.t + window_ns)]
        resp, unprobed = None, 0
        for f in post:
            if f.luma is None:
                unprobed += 1
                continue
            if abs(f.luma - base) > thr:
                resp = f
                break
        if resp is None:
            misses.append({"t": pr.t, "reason": "no change seen" if post else "no frames after press"})
            continue
        end, method = None, None
        if sc.timing and _is_xwayland(sc) and sc.mode not in VSYNC_MODES:
            # XWayland without vsync only reports a stale vblank: GPU done is the closest thing.
            if not resp.t_gpu:
                misses.append({"t": pr.t, "reason": "no gpu time"})
                continue
            end, method = resp.t_gpu, "gpu-done"
        elif sc.timing and _has_display_times(sc):
            for f in frames:
                if f.seq < resp.seq or f.t_shown is None:
                    continue
                if f.shown_kind == "drop" or not f.t_shown:
                    continue
                if f.shown_kind == "px" or sc.mode in VSYNC_MODES:
                    end, method = f.t_shown, "scanout"
                break
            if end is None:
                misses.append({"t": pr.t, "reason": "no display time"})
                continue
        else:
            if not resp.t_gpu:
                misses.append({"t": pr.t, "reason": "no gpu time"})
                continue
            end, method = resp.t_gpu, "gpu-done"
        lat = (end - pr.t) / 1e6
        if lat > log.window_ms:
            misses.append({"t": pr.t, "reason": "slower than the window"})
            continue
        s = {
            "t": pr.t,
            "latency_ms": round(lat, 3),
            "method": method,
            "frames_after": post.index(resp),
            "game_ms": round((resp.t_present - pr.t) / 1e6, 3),
            "luma": [round(base, 1), round(resp.luma, 1)],
        }
        if resp.t_gpu:
            s["gpu_ms"] = round((resp.t_gpu - resp.t_present) / 1e6, 3)
            if method == "scanout":
                s["display_ms"] = round((end - resp.t_gpu) / 1e6, 3)
        if unprobed:
            s["unprobed_before"] = unprobed
        samples.append(s)
    return {"samples": samples, "misses": misses, "presses": len(presses)}


def frame_context(log: Log) -> dict:
    """Frame interval and display refresh, from the frames that were logged."""
    pres, shown = [], []
    for sc in log.swapchains:
        frames = sorted(sc.frames.values(), key=lambda f: f.seq)
        for a, b in zip(frames, frames[1:]):
            if b.seq == a.seq + 1 and a.t_present and b.t_present:
                pres.append((b.t_present - a.t_present) / 1e6)
        times = sorted({f.t_shown for f in frames if f.t_shown and f.shown_kind == "px"})
        shown += [(b - a) / 1e6 for a, b in zip(times, times[1:]) if 0 < b - a < 100_000_000]
    out = {}
    if pres:
        fm = stats.median(pres)
        out["frame_ms"] = round(fm, 3)
        out["fps"] = round(1000 / fm, 1) if fm > 0 else None
    if len(shown) >= 5:
        out["scanout_interval_ms"] = round(stats.percentile(sorted(shown), 25), 3)
    return out


def summarize(result: dict) -> dict:
    lat = [s["latency_ms"] for s in result["samples"]]
    out = {"measured": len(lat), "missed": len(result["misses"]), "presses": result["presses"]}
    out.update(stats.describe(lat))
    for k in ("game_ms", "gpu_ms", "display_ms"):
        v = [s[k] for s in result["samples"] if k in s]
        if v:
            out[k.replace("_ms", "_median_ms")] = round(stats.median(v), 3)
    methods = {}
    for s in result["samples"]:
        methods[s["method"]] = methods.get(s["method"], 0) + 1
    out["methods"] = methods
    reasons = {}
    for m in result["misses"]:
        reasons[m["reason"]] = reasons.get(m["reason"], 0) + 1
    if reasons:
        out["miss_reasons"] = reasons
    return out


def find_logs(directory: str) -> list[str]:
    try:
        names = os.listdir(directory)
    except OSError:
        return []
    return sorted((os.path.join(directory, n) for n in names if n.endswith(".log")), key=os.path.getmtime)
