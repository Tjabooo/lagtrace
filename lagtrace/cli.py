"""lagtrace command line."""

from __future__ import annotations

import argparse
import datetime
import glob
import gzip
import json
import os
import platform
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from . import __version__, analysis, chart, stats
from .uinput import VirtualDevice, now_ns

LAYER_NAME = "VK_LAYER_LAGTRACE_latency"
DATA_HOME = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
RESULTS_DIR = os.path.join(DATA_HOME, "lagtrace", "results")
SHM_DIR = f"/dev/shm/lagtrace-{os.getuid()}"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Fail(Exception):
    pass


def say(msg: str = "") -> None:
    print(msg, flush=True)


def status(msg: str) -> None:
    if sys.stdout.isatty():
        print(f"\r\033[K{msg}", end="", flush=True)


# ---------------------------------------------------------------- setup checks
def manifest_dirs() -> list[str]:
    dirs = [os.path.join(DATA_HOME, "vulkan", "implicit_layer.d")]
    for d in (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":"):
        dirs.append(os.path.join(d, "vulkan", "implicit_layer.d"))
    dirs += ["/etc/xdg/vulkan/implicit_layer.d", "/etc/vulkan/implicit_layer.d"]
    return dirs


def installed_layer() -> str | None:
    for d in manifest_dirs():
        for path in glob.glob(os.path.join(d, "*.json")):
            try:
                with open(path) as fh:
                    m = json.load(fh)
            except (OSError, ValueError):
                continue
            layer = m.get("layer") or {}
            if layer.get("name") == LAYER_NAME:
                lib = layer.get("library_path", "")
                if not os.path.isabs(lib):
                    lib = os.path.join(d, lib)
                if os.path.exists(lib):
                    return path
    return None


def dev_layer_dir() -> str | None:
    d = os.path.join(REPO, "build")
    return d if os.path.exists(os.path.join(d, "VkLayer_lagtrace.x86_64.json")) else None


def with_layer(env: dict) -> dict:
    if installed_layer():
        return env
    dev = dev_layer_dir()
    if not dev:
        raise Fail("the lagtrace Vulkan layer isn't installed. Run `make install` in the lagtrace folder.")
    old = env.get("VK_ADD_IMPLICIT_LAYER_PATH")
    env["VK_ADD_IMPLICIT_LAYER_PATH"] = dev + (":" + old if old else "")
    return env


def find_bench() -> str:
    local = os.path.join(REPO, "build", "lagtrace-bench")
    if os.access(local, os.X_OK):
        return local  # running from a checkout
    p = shutil.which("lagtrace-bench")
    if p:
        return p
    raise Fail("lagtrace-bench not found. Run `make install` in the lagtrace folder.")


def input_readable() -> bool:
    for p in glob.glob("/dev/input/event*"):
        try:
            os.close(os.open(p, os.O_RDONLY | os.O_NONBLOCK))
            return True
        except OSError:
            continue
    return False


def check_access(manual: bool) -> None:
    if not input_readable():
        raise Fail("can't read /dev/input, so the layer can't see when you click.\n"
                   "  fix: sudo usermod -aG input $USER   (then log out and in)")
    if not manual and not os.access("/dev/uinput", os.W_OK):
        raise Fail("can't write /dev/uinput, so lagtrace can't click for you.\n"
                   "  fix: sudo usermod -aG input $USER, and if /dev/uinput isn't group input:\n"
                   "  echo 'KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"' | sudo tee /etc/udev/rules.d/60-uinput.rules\n"
                   "  sudo udevadm control --reload && sudo udevadm trigger /dev/uinput\n"
                   "  or measure your own clicks with --manual")


# ---------------------------------------------------------------- measuring
def parse_interval(s: str) -> tuple[float, float]:
    m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*(?:-\s*(\d+(?:\.\d+)?))?\s*", s)
    if not m:
        raise argparse.ArgumentTypeError("use MIN-MAX in ms, like 350-650")
    lo = float(m.group(1))
    hi = float(m.group(2)) if m.group(2) else lo
    return (min(lo, hi), max(lo, hi))


def read_logs(directory: str) -> list[analysis.Log]:
    return [analysis.read(p) for p in analysis.find_logs(directory)]


def wait_for_swapchain(directory: str, proc: subprocess.Popen, timeout: float) -> analysis.Log:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if proc.poll() is not None:
            raise Fail(f"the program exited (code {proc.returncode}) before it showed a frame")
        logs = read_logs(directory)
        # Wait for the program itself (not just a gamescope around it) to present.
        if any(lg.swapchains and not lg.comm.startswith("gamescope") for lg in logs):
            time.sleep(0.5)  # a nested gamescope's own window shows up around the same time
            return analysis.pick_main(read_logs(directory))
        time.sleep(0.2)
    log = analysis.pick_main(read_logs(directory))
    if not log:
        raise Fail("the lagtrace layer didn't load (no log). Is it installed? Try `lagtrace doctor`.")
    if not log.swapchains:
        raise Fail("the program never created a Vulkan swapchain (OpenGL and 32-bit games aren't supported)")
    return log


def countdown(seconds: float, what: str) -> None:
    end = time.monotonic() + seconds
    while True:
        left = end - time.monotonic()
        if left <= 0:
            break
        status(f"{what} in {left:.0f} s")
        time.sleep(min(0.25, left))
    status("")


def click_loop(dev: VirtualDevice, args, proc: subprocess.Popen | None) -> list[int]:
    lo, hi = args.interval
    times = []
    for i in range(args.clicks):
        if proc is not None and proc.poll() is not None:
            say("\nlagtrace: the program exited; stopping")
            break
        # Random gaps, so clicks land at every point of the refresh cycle.
        time.sleep(random.uniform(lo, hi) / 1000)
        times.append(dev.press())
        time.sleep(args.hold / 1000)
        dev.release()
        status(f"click {i + 1}/{args.clicks}")
    status("")
    return times


def manual_loop(directory: str | None, log_path: str | None, args, since: int, proc) -> None:
    say(f"Click {args.clicks} times (or press Ctrl+C when you're done). Leave a moment between clicks.")
    seen = 0
    while seen < args.clicks:
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.3)
        log = analysis.read(log_path) if log_path else analysis.pick_main(read_logs(directory))
        if log:
            seen = sum(1 for p in log.presses if p.t >= since and p.code == analysis.BTN_LEFT)
        status(f"clicks seen: {seen}/{args.clicks}")
    status("")


def stop(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


def system_info(log: analysis.Log) -> dict:
    gpu, _, driver = log.gpu.partition("|")
    return {"kernel": platform.release(), "session": os.environ.get("XDG_SESSION_TYPE", ""),
            "desktop": os.environ.get("XDG_CURRENT_DESKTOP", ""), "gpu": gpu.strip(), "driver": driver.strip()}


def build_result(log: analysis.Log, args, *, label: str, command: list | None, since: int, device: str | None,
                 cli_times: list[int], extra: dict | None = None) -> dict:
    res = analysis.analyze(log, device=device, code=None if device == analysis.VIRTUAL_PAD else analysis.BTN_LEFT,
                           since=since, threshold=args.threshold)
    summary = analysis.summarize(res)
    sc = max(log.swapchains, key=lambda s: len(s.frames)) if log.swapchains else None
    settings = {"clicks": args.clicks, "mode": "manual" if args.manual else "auto", "threshold": args.threshold,
                "probe": list(log.probe_opts)}
    if not args.manual:
        settings.update({"interval_ms": list(args.interval), "hold_ms": args.hold})
    if extra:
        settings.update(extra)
    if cli_times:
        kernel = [p.t for p in analysis._press_filter(log, device, None, since)]
        lag = [(k - c) / 1000 for k, c in zip(kernel, cli_times)]
        if lag:
            settings["inject_median_us"] = round(stats.median(lag), 1)
        settings["clicks_sent"] = len(cli_times)
    result = {
        "lagtrace": __version__,
        "label": label,
        "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "command": command,
        "app": log.comm,
        "system": system_info(log),
        "swapchain": None if sc is None else {
            "width": sc.width, "height": sc.height, "format": analysis.FORMATS.get(sc.fmt, str(sc.fmt)),
            "present_mode": sc.mode_name, "display_timing": sc.timing, "probe": list(sc.probe_rect) if sc.probe else None},
        "settings": settings,
        "summary": summary,
        "frames": analysis.frame_context(log),
        "samples": res["samples"],
        "misses": res["misses"],
        "warnings": log.warnings,
        "inside_gamescope": log.gamescope,
    }
    return result


def slug(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")[:48] or "run"


def save(result: dict, log_path: str | None, out: str | None) -> str:
    if out:
        path = out
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    else:
        os.makedirs(RESULTS_DIR, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
        path = os.path.join(RESULTS_DIR, f"{stamp}_{slug(result['label'])}.json")
    if log_path and os.path.exists(log_path):
        raw = path[:-5] + ".log.gz" if path.endswith(".json") else path + ".log.gz"
        with open(log_path, "rb") as src, gzip.open(raw, "wb") as dst:
            shutil.copyfileobj(src, dst)
        result["raw_log"] = os.path.basename(raw)
    with open(path, "w") as fh:
        json.dump(result, fh, indent=1)
        fh.write("\n")
    return path


def end_point_text(methods: dict) -> str:
    if not methods:
        return "-"
    if set(methods) == {"scanout"}:
        return "first pixel out (VK_EXT_present_timing)"
    if set(methods) == {"gpu-done"}:
        return "GPU done (no display timing here, so the compositor and display queue are NOT included)"
    return "mixed: " + ", ".join(f"{k} {v}" for k, v in methods.items())


def print_summary(result: dict, path: str | None = None) -> None:
    s = result["summary"]
    miss = ""
    if s.get("miss_reasons"):
        miss = " (" + ", ".join(f"{k}: {v}" for k, v in s["miss_reasons"].items()) + ")"
    say(f"lagtrace · {result['label']} · {s['presses']} clicks → {s['measured']} measured, {s['missed']} missed{miss}")
    if s.get("n"):
        say(f"  click to photon   median {s['median']:.1f} ms   p5 {s['p5']:.1f}   p95 {s['p95']:.1f}   "
            f"mean {s['mean']:.1f} ± {s['sd']:.1f}")
        parts = []
        if "game_median_ms" in s:
            parts.append(f"click → present {s['game_median_ms']:.1f} ms")
        if "gpu_median_ms" in s:
            parts.append(f"GPU {s['gpu_median_ms']:.1f} ms")
        if "display_median_ms" in s:
            parts.append(f"compositor + display {s['display_median_ms']:.1f} ms")
        if parts:
            say("  where it went     " + " · ".join(parts) + "   (medians)")
        say(f"  end point         {end_point_text(s.get('methods', {}))}")
    fr = result.get("frames") or {}
    if fr.get("fps"):
        disp = f", display every {fr['scanout_interval_ms']:.2f} ms" if fr.get("scanout_interval_ms") else ""
        say(f"  frames            {fr['fps']:.0f} fps{disp}")
    sc = result.get("swapchain") or {}
    if sc:
        say(f"  swapchain         {sc['width']}x{sc['height']} {sc['present_mode']}, {result['app']} on "
            f"{result['system'].get('gpu') or '?'}")
    if result.get("inside_gamescope"):
        say("  note              inside gamescope: display times are gamescope's. In a gamescope session\n"
            "                    (Steam Deck, SteamOS) that's the real flip; in a nested gamescope (a window on\n"
            "                    your desktop) your desktop's compositing comes after it and isn't included.")
    for w in result.get("warnings") or []:
        say(f"  layer warning     {w}")
    if path:
        say(f"  saved             {path}")


def diagnose(result: dict, log: analysis.Log, device: str | None) -> None:
    s = result["summary"]
    if s["presses"] == 0:
        if device:
            say("lagtrace: the layer never saw the virtual mouse's clicks. Are you in the input group? (lagtrace doctor)")
        else:
            say("lagtrace: the layer saw no left clicks.")
        return
    if s["measured"] < s["presses"] / 2:
        reasons = s.get("miss_reasons", {})
        if reasons.get("no frames", 0) + reasons.get("no frames after press", 0) >= s["missed"] / 2:
            say("lagtrace: the program presented too few frames around the clicks. Is its window visible?\n"
                "  (Compositors throttle hidden windows; keep it fullscreen on a visible workspace.)")
        elif reasons.get("no change seen", 0) >= s["missed"] / 2:
            say("lagtrace: most clicks didn't change the probe region. Make sure the window has focus (fullscreen),\n"
                "  and that something visible happens where lagtrace looks (--probe X,Y,SIZE; centre by default).")
        elif reasons.get("no display time"):
            say("lagtrace: the driver didn't report display times for some frames.")


def measure(args, *, label: str, command: list | None = None, attach_log: str | None = None,
            extra: dict | None = None, direct: bool = False) -> int:
    check_access(args.manual)
    tmp = None
    proc = None
    dev = None
    device = None if args.manual else (analysis.VIRTUAL_PAD if direct else analysis.VIRTUAL_MOUSE)
    cli_times: list[int] = []
    since = 0
    try:
        if not args.manual:
            dev = VirtualDevice(device, "pad" if direct else "mouse")
            time.sleep(0.6)  # let udev, the compositor and the layer pick it up
        if command:
            tmp = tempfile.mkdtemp(prefix="lagtrace-")
            env = dict(os.environ, LAGTRACE="1", LAGTRACE_LOG=os.path.join(tmp, "%p.log"))
            if args.probe:
                env["LAGTRACE_PROBE"] = args.probe
            env = with_layer(env)
            try:
                proc = subprocess.Popen(command, env=env, start_new_session=True)
            except OSError as e:
                raise Fail(f"can't start {command[0]}: {e.strerror}")
            log = wait_for_swapchain(tmp, proc, timeout=40)
        else:
            log = analysis.read(attach_log)
        sc = log.swapchains[-1] if log.swapchains else None
        if sc is None:
            raise Fail(f"{log.comm} hasn't shown a frame yet")
        if log.comm.startswith("gamescope"):
            say("lagtrace: nested gamescope: measuring at gamescope's own window, which the desktop shows")
        say(f"lagtrace: {log.comm} is presenting {sc.width}x{sc.height} ({sc.mode_name}), "
            f"display timing {'on' if sc.timing else 'OFF (end point will be GPU done)'}")
        if not sc.probe:
            raise Fail("the layer can't read this swapchain's pixels (unsupported format or usage)")
        if attach_log:
            say("Switch to the game window now and keep your hands off the mouse." if not args.manual else
                "Switch to the game window now.")
        countdown(args.warmup, "measuring starts")
        since = now_ns()
        if args.manual:
            manual_loop(tmp, attach_log, args, since, proc)
        else:
            cli_times = click_loop(dev, args, proc)
        time.sleep(max(0.5, log.window_ms / 1000 + 0.2))  # the last click's frames + log flush
    except KeyboardInterrupt:
        status("")
        say("lagtrace: stopped")
    finally:
        if dev:
            dev.close()
        if proc is not None and not args.keep_open:
            stop(proc)
    if command:
        logs = read_logs(tmp)
        log = analysis.pick_main(logs)
        log_path = log.path if log else None
    else:
        log = analysis.read(attach_log)
        log_path = attach_log
    if not since:
        raise Fail("nothing measured")
    result = build_result(log, args, label=label, command=command, since=since, device=device, cli_times=cli_times,
                          extra=extra)
    asked = ((extra or {}).get("bench") or {}).get("present_mode")
    got = (result.get("swapchain") or {}).get("present_mode")
    if asked and got and asked != got:
        say(f"lagtrace: asked for {asked}, but this compositor/driver gave {got}")
        if not args.label:
            result["label"] = result["label"].replace(asked, f"{got} (asked {asked})", 1)
    path = save(result, log_path, args.out)
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    print_summary(result, path)
    diagnose(result, log, device)
    return 0 if result["summary"]["measured"] else 1


# ---------------------------------------------------------------- commands
def cmd_run(args) -> int:
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise Fail("give the program to run after --, like: lagtrace run -- vkcube")
    return measure(args, label=args.label or os.path.basename(command[0]), command=command)


def cmd_bench(args) -> int:
    bench = [find_bench(), "--present-mode", args.present_mode]
    if args.x11:
        bench.append("--x11")
    if args.windowed:
        bench.append("--windowed")
    if args.fps_cap:
        bench += ["--fps-cap", str(args.fps_cap)]
    if args.load_ms:
        bench += ["--load-ms", str(args.load_ms)]
    if args.images:
        bench += ["--images", str(args.images)]
    if args.direct:
        bench += ["--evdev", analysis.VIRTUAL_PAD, "--no-grab"]
        if args.manual:
            raise Fail("--direct uses lagtrace's own virtual gamepad; it can't be combined with --manual")
    command = bench
    if args.gamescope is not None:
        command = ["gamescope"] + args.gamescope.split() + ["--"] + bench
        if "headless" not in args.gamescope and "drm" not in args.gamescope:
            say("lagtrace: nested gamescope hands its frames to your desktop without a Vulkan swapchain, so the\n"
                "  end point is gamescope's own present; your desktop's compositing after it isn't included.")
    parts = [args.present_mode]
    if args.x11:
        parts.append("x11")
    if args.gamescope is not None:
        parts.append("gamescope")
    if args.fps_cap:
        parts.append(f"cap {args.fps_cap:g}")
    if args.load_ms:
        parts.append(f"load {args.load_ms:g} ms")
    if args.direct:
        parts.append("direct input")
    label = args.label or "bench " + " ".join(parts)
    extra = {"bench": {"present_mode": args.present_mode, "x11": args.x11, "gamescope": args.gamescope,
                       "fps_cap": args.fps_cap, "load_ms": args.load_ms, "direct": args.direct}}
    return measure(args, label=label, command=command, extra=extra, direct=args.direct)


SUITE = [
    ("Wayland · vsync", ["--present-mode", "fifo"]),
    ("Wayland · vsync, {cap} fps cap", ["--present-mode", "fifo", "--fps-cap", "{cap}"]),
    ("Wayland · mailbox", ["--present-mode", "mailbox"]),
    ("XWayland · vsync", ["--present-mode", "fifo", "--x11"]),
]


def cmd_suite(args) -> int:
    """The standard set: the same bench through each path, one chart to share."""
    parser = build_parser()
    paths = []
    cap = None
    for label, extra in SUITE:
        if "{cap}" in label:
            if not cap:
                say(f"lagtrace: skipping '{label}' (no refresh rate from the first run)")
                continue
            label = label.format(cap=cap)
            extra = [x.format(cap=cap) for x in extra]
        argv = ["bench", *extra, "--label", label, "--clicks", str(args.clicks), "--warmup", str(args.warmup)]
        if args.direct:
            argv.append("--direct")
        if args.windowed:
            argv.append("--windowed")
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
        out = os.path.join(RESULTS_DIR, f"{stamp}_suite_{slug(label)}.json")
        argv += ["-o", out]
        say(f"\n== {label}")
        sub = parser.parse_args(argv)
        try:
            sub.func(sub)
        except Fail as e:
            say(f"lagtrace: {label} failed: {e}")
        if os.path.exists(out):
            paths.append(out)
            if cap is None:
                # The classic advice: cap 3 fps under the refresh rate so the vsync queue never fills.
                interval = (load_result(out).get("frames") or {}).get("scanout_interval_ms")
                if interval:
                    cap = max(30, round(1000 / interval) - 3)
    if len(paths) < 2:
        raise Fail("not enough runs worked to compare")
    png = args.png or os.path.join(RESULTS_DIR, datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S") + "_suite.png")
    say("")
    cargs = parser.parse_args(["compare", *paths, "--png", png] + (["--light"] if args.light else []))
    return cargs.func(cargs)


def live_logs() -> list[str]:
    out = []
    for p in analysis.find_logs(SHM_DIR):
        try:
            pid = int(os.path.basename(p).split(".")[0])
            os.kill(pid, 0)
            out.append(p)
        except (ValueError, ProcessLookupError):
            continue
        except PermissionError:
            out.append(p)
    return out


def cmd_attach(args) -> int:
    logs = live_logs()
    if args.pid:
        logs = [p for p in logs if os.path.basename(p) == f"{args.pid}.log"]
    logs = [p for p in logs if analysis.read(p).swapchains]
    if not logs:
        raise Fail("no running game has the lagtrace layer on. Start it with LAGTRACE=1, e.g. in Steam's\n"
                   "  launch options: LAGTRACE=1 %command%")
    path = logs[-1]
    log = analysis.read(path)
    say(f"lagtrace: attaching to {log.comm} (pid {log.pid})")
    return measure(args, label=args.label or log.comm, attach_log=path)


def cmd_analyze(args) -> int:
    for p in args.logs:
        opener = gzip.open if p.endswith(".gz") else open
        with opener(p, "rt", encoding="utf-8", errors="replace") as fh:
            log = analysis.parse(fh.read(), p)
        args.clicks, args.manual = 0, True
        res = build_result(log, args, label=args.label or log.comm, command=None, since=0, device=args.device,
                           cli_times=[])
        res["settings"]["clicks"] = res["summary"]["presses"]
        path = save(res, None, args.out) if args.out else None
        print_summary(res, path)
    return 0


def load_result(path: str) -> dict:
    with open(path) as fh:
        return json.load(fh)


def chart_context(r: dict) -> str:
    sysi = r.get("system", {})
    bits = [sysi.get("gpu"), sysi.get("driver"), sysi.get("desktop")]
    sc = r.get("swapchain") or {}
    if sc:
        bits.append(f"{sc['width']}x{sc['height']}")
    fr = r.get("frames") or {}
    if fr.get("scanout_interval_ms"):
        bits.append(f"display {1000 / fr['scanout_interval_ms']:.0f} Hz")
    return " · ".join(b for b in bits if b)


def write_chart(svg: str, svg_path: str | None, png_path: str | None) -> None:
    if svg_path:
        with open(svg_path, "w") as fh:
            fh.write(svg)
        say(f"chart: {svg_path}")
    if png_path:
        tmp_svg = svg_path or png_path + ".svg"
        if not svg_path:
            with open(tmp_svg, "w") as fh:
                fh.write(svg)
        ok = chart.to_png(tmp_svg, png_path)
        if not svg_path:
            os.unlink(tmp_svg)
        if not ok:
            raise Fail("PNG needs rsvg-convert (librsvg) or ImageMagick; use --svg instead")
        say(f"chart: {png_path}")


def cmd_show(args) -> int:
    r = load_result(args.result)
    print_summary(r)
    if args.svg or args.png:
        lat = [s["latency_ms"] for s in r["samples"]]
        if not lat:
            raise Fail("no samples to draw")
        s = r["summary"]
        stages = ""
        if "display_median_ms" in s:
            stages = (f"Medians: click → frame presented {s['game_median_ms']:.1f} ms · GPU {s['gpu_median_ms']:.1f} ms · "
                      f"compositor + display {s['display_median_ms']:.1f} ms")
        svg = chart.histogram(lat, label=r["label"], theme="light" if args.light else "dark", context=chart_context(r),
                              stages=stages)
        write_chart(svg, args.svg, args.png)
    return 0


def recent_results(n: int) -> list[str]:
    files = sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json")), key=os.path.getmtime)
    return files[-n:]


def cmd_compare(args) -> int:
    paths = list(args.results)
    if args.last:
        paths += recent_results(args.last)
    if len(paths) < 2:
        raise Fail("compare needs at least two results (files, or --last N)")
    runs = [load_result(p) for p in paths]
    for r, p in zip(runs, paths):
        if not r["samples"]:
            raise Fail(f"{p} has no samples")
    ref = [s["latency_ms"] for s in runs[0]["samples"]]
    width = max(len(r["label"]) for r in runs) + 2
    say(f"{'':{width}}{'n':>5} {'median':>8} {'p5':>7} {'p95':>7}   vs {runs[0]['label']}")
    for i, r in enumerate(runs):
        lat = [s["latency_ms"] for s in r["samples"]]
        d = stats.describe(lat)
        line = f"{r['label']:{width}}{d['n']:>5} {d['median']:>8.2f} {d['p5']:>7.2f} {d['p95']:>7.2f}"
        if i:
            diff, lo, hi = stats.bootstrap_median_diff(ref, lat)
            _, p = stats.mann_whitney(ref, lat)
            ptxt = "p < 0.001" if p < 0.001 else f"p = {p:.3f}"
            line += f"   {diff:+.2f} ms (95% CI {lo:+.2f} to {hi:+.2f}), {ptxt}"
        say(line)
    methods = {m for r in runs for m in r["summary"].get("methods", {})}
    if len(methods) > 1 or methods == {"gpu-done"}:
        say("note: not every run ends at the display (some used GPU done); compare those with care")
    if args.svg or args.png:
        data = []
        for r in runs:
            lat = [s["latency_ms"] for s in r["samples"]]
            fr = r.get("frames") or {}
            note = [f"n = {len(lat)}", (r.get("swapchain") or {}).get("present_mode", "")]
            if fr.get("fps"):
                note.append(f"{fr['fps']:.0f} fps")
            data.append({"label": r["label"], "latencies": lat, "note": " · ".join(x for x in note if x)})
        svg = chart.compare(data, title=args.title, theme="light" if args.light else "dark",
                            context=chart_context(runs[0]))
        write_chart(svg, args.svg, args.png)
    return 0


def cmd_list(args) -> int:
    files = sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json")), key=os.path.getmtime, reverse=True)[:args.n]
    if not files:
        say(f"no results yet in {RESULTS_DIR}")
    for p in files:
        try:
            r = load_result(p)
        except (OSError, ValueError):
            continue
        s = r.get("summary", {})
        med = f"{s['median']:7.2f} ms" if s.get("n") else "      -   "
        say(f"{med}  n={s.get('n', 0):<4} {r.get('label', '?'):32} {p}")
    return 0


def cmd_doctor(args) -> int:
    ok = True

    def line(good, what, detail=""):
        nonlocal ok
        ok = ok and good is not False
        mark = "ok  " if good else ("??  " if good is None else "FAIL")
        say(f"  {mark} {what}{'  ' + detail if detail else ''}")

    say(f"lagtrace {__version__}")
    inst = installed_layer()
    dev = dev_layer_dir()
    line(bool(inst or dev), "Vulkan layer", inst or (f"not installed; using the build in {dev}" if dev else
                                                     "missing: run `make install`"))
    try:
        line(True, "bench", find_bench())
    except Fail as e:
        line(False, "bench", str(e))
    line(input_readable(), "read /dev/input (click times)", "" if input_readable() else "sudo usermod -aG input $USER, log in again")
    line(os.access("/dev/uinput", os.W_OK), "write /dev/uinput (automatic clicks)",
         "" if os.access("/dev/uinput", os.W_OK) else "see README (or use --manual)")
    vi = shutil.which("vulkaninfo")
    if vi:
        out = subprocess.run([vi], capture_output=True, text=True, timeout=30).stdout
        has = "VK_EXT_present_timing" in out
        line(has if has else None, "VK_EXT_present_timing (display times)",
             "" if has else "not offered: results end at 'GPU done' (Mesa 26.1+ RADV/ANV have it)")
    else:
        line(None, "VK_EXT_present_timing", "install vulkan-tools to check")
    say(f"  info session {os.environ.get('XDG_SESSION_TYPE', '?')}, desktop {os.environ.get('XDG_CURRENT_DESKTOP', '?')}")
    if args.live:
        tmp = tempfile.mkdtemp(prefix="lagtrace-doctor-")
        env = with_layer(dict(os.environ, LAGTRACE="1", LAGTRACE_LOG=os.path.join(tmp, "%p.log")))
        proc = subprocess.Popen([find_bench(), "--windowed", "--no-grab"], env=env, start_new_session=True,
                                stdout=subprocess.DEVNULL)
        try:
            log = wait_for_swapchain(tmp, proc, timeout=15)
            time.sleep(1)
            sc = log.swapchains[-1]
            line(sc.probe, "layer reads pixels", f"{sc.width}x{sc.height} {analysis.FORMATS.get(sc.fmt, sc.fmt)}")
            line(sc.timing if sc.timing else None, "layer gets display times", "" if sc.timing else "GPU-done fallback")
            line(bool(log.devices), "layer sees input devices", ", ".join(sorted(set(log.devices.values())))[:80])
        except Fail as e:
            line(False, "live test", str(e))
        finally:
            stop(proc)
            shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


# ---------------------------------------------------------------- argument parsing
def add_measure_opts(p: argparse.ArgumentParser, warmup: float) -> None:
    g = p.add_argument_group("measuring")
    g.add_argument("--clicks", type=int, default=100, help="how many clicks (default 100)")
    g.add_argument("--interval", type=parse_interval, default=(350.0, 650.0), metavar="MIN-MAX",
                   help="random gap between clicks in ms (default 350-650)")
    g.add_argument("--hold", type=float, default=120.0, metavar="MS", help="how long each click is held (default 120)")
    g.add_argument("--warmup", type=float, default=warmup, metavar="S", help=f"wait before the first click (default {warmup:g})")
    g.add_argument("--manual", action="store_true", help="you click (any mouse) instead of lagtrace's virtual mouse")
    g.add_argument("--probe", metavar="X,Y[,SIZE]",
                   help="where to watch for the change: centre as fractions of the frame, size in px (default 0.5,0.5,64)")
    g.add_argument("--threshold", type=float, default=8.0, help="luma change (0-255) that counts as a response (default 8)")
    g.add_argument("--label", help="name for this run")
    g.add_argument("-o", "--out", help="result file (default ~/.local/share/lagtrace/results/...)")
    g.add_argument("--keep-open", action="store_true", help="don't close the program afterwards")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="lagtrace", description="Click-to-photon latency for Linux games, measured in software.")
    ap.add_argument("--version", action="version", version=f"lagtrace {__version__}")
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND")

    p = sub.add_parser("bench", help="measure your desktop's latency with the built-in test app")
    p.add_argument("--present-mode", default="fifo", choices=["fifo", "fifo-relaxed", "mailbox", "immediate"])
    p.add_argument("--x11", action="store_true", help="run the bench through XWayland")
    p.add_argument("--gamescope", metavar="ARGS", help='run the bench inside gamescope with these arguments, e.g. --gamescope="-f -r 240"')
    p.add_argument("--fps-cap", type=float, metavar="N", help="frame-rate cap (sleep-based, like an in-game limiter)")
    p.add_argument("--load-ms", type=float, metavar="MS", help="CPU work per frame after reading input, like a game")
    p.add_argument("--images", type=int, metavar="N", help="swapchain image count")
    p.add_argument("--windowed", action="store_true", help="don't go fullscreen")
    p.add_argument("--direct", action="store_true",
                   help="the bench reads a virtual gamepad straight from /dev/input, skipping the compositor's input path")
    add_measure_opts(p, 3)
    p.set_defaults(func=cmd_bench)

    p = sub.add_parser("suite", help="bench Wayland (vsync, capped, mailbox) and XWayland into one chart")
    p.add_argument("--clicks", type=int, default=60, help="clicks per run (default 60)")
    p.add_argument("--warmup", type=float, default=3)
    p.add_argument("--direct", action="store_true",
                   help="the bench reads a virtual gamepad itself, skipping the compositor's input path")
    p.add_argument("--windowed", action="store_true", help="don't go fullscreen")
    p.add_argument("--png", help="chart file (default: next to the results)")
    p.add_argument("--light", action="store_true", help="light theme")
    p.set_defaults(func=cmd_suite)

    p = sub.add_parser("run", help="start a Vulkan program and measure it: lagtrace run [options] -- COMMAND")
    p.add_argument("command", nargs=argparse.REMAINDER)
    add_measure_opts(p, 5)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("attach", help="measure a running game started with LAGTRACE=1 (e.g. from Steam)")
    p.add_argument("--pid", type=int, help="which game (default: the newest)")
    add_measure_opts(p, 5)
    p.set_defaults(func=cmd_attach)

    p = sub.add_parser("show", help="print a saved result, optionally as a chart")
    p.add_argument("result")
    p.add_argument("--svg")
    p.add_argument("--png")
    p.add_argument("--light", action="store_true", help="light theme")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("compare", help="compare saved results (statistics + chart)")
    p.add_argument("results", nargs="*")
    p.add_argument("--last", type=int, metavar="N", help="add your N most recent results")
    p.add_argument("--svg")
    p.add_argument("--png")
    p.add_argument("--light", action="store_true", help="light theme")
    p.add_argument("--title", default="Click-to-photon latency")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("list", help="list saved results")
    p.add_argument("-n", type=int, default=20)
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("analyze", help="re-analyze raw layer logs (.log or .log.gz)")
    p.add_argument("logs", nargs="+")
    p.add_argument("--device", help="only presses from input devices whose name contains this")
    p.add_argument("--threshold", type=float, default=8.0)
    p.add_argument("--label")
    p.add_argument("-o", "--out")
    p.set_defaults(func=cmd_analyze)

    p = sub.add_parser("doctor", help="check that everything lagtrace needs is in place")
    p.add_argument("--live", action="store_true", help="also open the bench in a window for a moment and check the layer")
    p.set_defaults(func=cmd_doctor)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    try:
        return args.func(args)
    except Fail as e:
        status("")
        print(f"lagtrace: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
