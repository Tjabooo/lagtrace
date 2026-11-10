"""SVG charts (standard library only); PNG via rsvg-convert or ImageMagick if installed.

compare(): one row per run, every click a dot, a tick at the median and a bar under the
dots from the 5th to the 95th percentile. histogram(): one run's distribution.
"""

from __future__ import annotations

import math
import random
import shutil
import subprocess
from xml.sax.saxutils import escape

from . import HOMEPAGE, stats

THEMES = {
    "dark": {"page": "#0d0d0d", "surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
             "grid": "#2c2c2a", "axis": "#383835", "series": "#3987e5"},
    "light": {"page": "#f9f9f7", "surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
              "grid": "#e1e0d9", "axis": "#c3c2b7", "series": "#2a78d6"},
}
FONT = "system-ui, -apple-system, 'Segoe UI', Inter, 'Noto Sans', 'DejaVu Sans', sans-serif"


def _nice_step(span: float, target: int) -> float:
    raw = span / max(target, 1)
    mag = 10 ** math.floor(math.log10(raw)) if raw > 0 else 1
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


def _ticks(lo: float, hi: float, target: int = 6):
    step = _nice_step(hi - lo, target)
    start = math.floor(lo / step) * step
    end = math.ceil(hi / step) * step
    n = int(round((end - start) / step))
    return [round(start + i * step, 6) for i in range(n + 1)], step


def _fmt(v: float) -> str:
    return f"{v:.0f}" if abs(v - round(v)) < 1e-9 else f"{v:.1f}"


def _text(x, y, s, size, fill, weight=400, anchor="start", extra=""):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}"{extra}>{escape(str(s))}</text>')


def _frame(w: int, h: int, t: dict, title: str, subtitle: str, context: str) -> list[str]:
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
           f'font-family="{FONT}">',
           f'<rect width="{w}" height="{h}" fill="{t["page"]}"/>',
           f'<rect x="12" y="12" width="{w - 24}" height="{h - 24}" rx="12" fill="{t["surface"]}"/>',
           _text(40, 58, title, 24, t["ink"], 600)]
    if subtitle:
        out.append(_text(40, 84, subtitle, 14, t["ink2"]))
    if context:
        out.append(_text(40, 106, context, 13, t["muted"]))
    out.append(_text(w - 40, h - 30, f"measured in software with lagtrace · {HOMEPAGE}", 12, t["muted"], anchor="end"))
    return out


def compare(runs: list[dict], *, title: str = "Click-to-photon latency", theme: str = "dark",
            context: str = "") -> str:
    """runs: [{"label", "latencies": [...], "note": "n=98 · fifo"}]; the first is the reference."""
    t = THEMES[theme]
    w, row_h = 1200, 76
    left, right = 300, 210
    top = 150
    h = top + row_h * len(runs) + 96
    x0, x1 = left, w - right
    allv = [v for r in runs for v in r["latencies"]]
    lo, hi = min(allv), max(allv)
    pad = max((hi - lo) * 0.04, 0.5)
    ticks, _ = _ticks(max(0.0, lo - pad), hi + pad)
    vmin, vmax = ticks[0], ticks[-1]
    sx = lambda v: x0 + (v - vmin) / (vmax - vmin) * (x1 - x0)  # noqa: E731
    out = _frame(w, h, t, title, "Lower is better · each dot is one click · tick = median · bar = 5th–95th percentile",
                 context)
    plot_bottom = top + row_h * len(runs)
    for tv in ticks:  # recessive hairline grid + tick labels
        x = sx(tv)
        out.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{plot_bottom}" stroke="{t["grid"]}" stroke-width="1"/>')
        out.append(_text(x, plot_bottom + 22, f"{_fmt(tv)}", 12, t["muted"], anchor="middle",
                         extra=' style="font-variant-numeric: tabular-nums"'))
    out.append(_text((x0 + x1) / 2, plot_bottom + 44, "milliseconds from click to first pixel out", 12, t["muted"], anchor="middle"))
    ref_med = stats.median(runs[0]["latencies"]) if runs[0]["latencies"] else None
    rng = random.Random(7)
    for i, r in enumerate(runs):
        cy = top + row_h * i + row_h / 2 - 6
        vals = r["latencies"]
        s = sorted(vals)
        med = stats.median(vals)
        p5, p95 = stats.percentile(s, 5), stats.percentile(s, 95)
        out.append(_text(40, cy + 1, r["label"], 16, t["ink"], 600))
        if r.get("note"):
            out.append(_text(40, cy + 21, r["note"], 12, t["ink2"]))
        # 5th-95th percentile bar under the dots
        out.append(f'<line x1="{sx(p5):.1f}" y1="{cy + 22}" x2="{sx(p95):.1f}" y2="{cy + 22}" stroke="{t["muted"]}" '
                   f'stroke-width="2" stroke-linecap="round"/>')
        for v in vals:  # dots with a 2px surface ring, jittered inside the band
            y = cy + rng.uniform(-13, 13)
            out.append(f'<circle cx="{sx(v):.1f}" cy="{y:.1f}" r="4" fill="{t["series"]}" fill-opacity="0.9" '
                       f'stroke="{t["surface"]}" stroke-width="2"><title>{v:.2f} ms</title></circle>')
        out.append(f'<line x1="{sx(med):.1f}" y1="{cy - 20}" x2="{sx(med):.1f}" y2="{cy + 16}" stroke="{t["ink"]}" '
                   f'stroke-width="2" stroke-linecap="round"/>')
        out.append(_text(w - 40, cy + 1, f"{med:.1f} ms", 20, t["ink"], 600, anchor="end"))
        if i > 0 and ref_med is not None:
            d = med - ref_med
            sign = "±" if abs(d) < 0.05 else "+" if d > 0 else "−"
            out.append(_text(w - 40, cy + 21, f"{sign}{abs(d):.1f} ms vs {runs[0]['label']}"[:34], 12, t["ink2"], anchor="end"))
        else:
            out.append(_text(w - 40, cy + 21, f"p95 {p95:.1f} ms", 12, t["ink2"], anchor="end"))
    out.append("</svg>")
    return "\n".join(out)


def histogram(latencies: list[float], *, label: str, theme: str = "dark", context: str = "",
              stages: str = "") -> str:
    t = THEMES[theme]
    w, h = 1200, 560
    x0, x1, top, bottom = 90, w - 60, 170, h - 110
    s = sorted(latencies)
    med, p95 = stats.percentile(s, 50), stats.percentile(s, 95)
    span = max(s[-1] - s[0], 1.0)
    bw = _nice_step(span, 32)
    start = math.floor(s[0] / bw) * bw
    nb = max(1, int(math.ceil((s[-1] - start) / bw + 1e-9)))
    counts = [0] * nb
    for v in s:
        counts[min(nb - 1, int((v - start) / bw))] += 1
    ticks, _ = _ticks(start, start + nb * bw, 8)
    vmin, vmax = ticks[0], ticks[-1]
    sx = lambda v: x0 + (v - vmin) / (vmax - vmin) * (x1 - x0)  # noqa: E731
    ymax_ticks, _ = _ticks(0, max(counts), 4)
    cmax = ymax_ticks[-1] or 1
    sy = lambda c: bottom - c / cmax * (bottom - top)  # noqa: E731
    sub = f"{label} · {len(s)} clicks · median {med:.1f} ms · 95th percentile {p95:.1f} ms"
    out = _frame(w, h, t, "Click-to-photon latency", sub, context)
    for c in ymax_ticks:
        y = sy(c)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{t["grid"] if c else t["axis"]}" stroke-width="1"/>')
        out.append(_text(x0 - 12, y + 4, _fmt(c), 12, t["muted"], anchor="end", extra=' style="font-variant-numeric: tabular-nums"'))
    for tv in ticks:
        out.append(_text(sx(tv), bottom + 22, _fmt(tv), 12, t["muted"], anchor="middle",
                         extra=' style="font-variant-numeric: tabular-nums"'))
    out.append(_text((x0 + x1) / 2, bottom + 46, "milliseconds from click to first pixel out", 12, t["muted"], anchor="middle"))
    out.append(_text(x0 - 12, top - 14, "clicks", 12, t["muted"], anchor="end"))
    slot = sx(start + bw) - sx(start)
    bar = min(24.0, max(1.0, slot - 2))
    for k, c in enumerate(counts):
        if not c:
            continue
        cx = sx(start + (k + 0.5) * bw)
        xl, xr, yt = cx - bar / 2, cx + bar / 2, sy(c)
        r = min(4.0, bar / 2, bottom - yt)
        # 4px rounded data end, square at the baseline
        out.append(f'<path d="M{xl:.1f},{bottom} V{yt + r:.1f} Q{xl:.1f},{yt:.1f} {xl + r:.1f},{yt:.1f} '
                   f'H{xr - r:.1f} Q{xr:.1f},{yt:.1f} {xr:.1f},{yt + r:.1f} V{bottom} Z" fill="{t["series"]}">'
                   f'<title>{start + k * bw:.1f}–{start + (k + 1) * bw:.1f} ms: {c}</title></path>')
    for v, name, color, wdt in ((med, f"median {med:.1f}", t["ink"], 2), (p95, f"p95 {p95:.1f}", t["ink2"], 1)):
        x = sx(v)
        out.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{bottom}" stroke="{color}" stroke-width="{wdt}"/>')
        out.append(_text(x + 6, top + 6, name, 13, color, 600 if wdt == 2 else 400))
    if stages:
        out.append(_text(40, h - 30, stages, 12, t["ink2"]))
    out.append("</svg>")
    return "\n".join(out)


def to_png(svg_path: str, png_path: str, scale: float = 2.0) -> bool:
    if shutil.which("rsvg-convert"):
        cmd = ["rsvg-convert", "-z", str(scale), "-o", png_path, svg_path]
    elif shutil.which("magick"):
        cmd = ["magick", "-density", str(int(96 * scale)), svg_path, png_path]
    else:
        return False
    return subprocess.run(cmd, capture_output=True).returncode == 0
