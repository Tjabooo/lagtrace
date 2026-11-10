import os
import sys
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lagtrace import analysis, chart, stats, uinput  # noqa: E402

MS = 1_000_000
FRAME = 4_166_667


def synth(presses, *, n=400, resp_delay=1, display=10 * MS, timing=True, kind="px", noise=None, respond=True, mode=2):
    """A fake layer log: a 240 fps app that turns white `resp_delay` frames after each press."""
    t0 = 1_000_000_000
    lines = ["# lagtrace log 1", "I 42 1 fakegame", "O 0.1.0 0.5 0.5 64 300", "G Fake GPU|fakedrv 1.0",
             "D 0 lagtrace virtual mouse", "D 1 Some Real Mouse",
             f"W 0 1920 1080 44 {mode} {1 if timing else 0} 1 928 508 64 64 3"]
    for dev, t in presses:
        lines.append(f"C {t} {dev} 272")
    white_from = {}
    for dev, t in presses:
        k = (t - t0) // FRAME + 1  # first frame presented after the press
        white_from[k + resp_delay] = True
    lit = False
    for k in range(n):
        tp = t0 + k * FRAME
        if k in white_from and respond:
            lit = True
        elif any(k == w + 20 for w in white_from):
            lit = False
        luma = 255.0 if lit else 0.0
        if noise:
            luma += noise[k % len(noise)]
        lines.append(f"F 0 {k} {tp}")
        lines.append(f"P 0 {k} {luma:.2f} {tp + MS // 2}")
        if timing:
            lines.append(f"S 0 {k} {tp + display} {kind}")
    return "\n".join(lines) + "\n"


class AnalysisTest(unittest.TestCase):
    def test_latency_is_press_to_first_pixel_out(self):
        press = 1_000_000_000 + 50 * FRAME + FRAME // 2
        log = analysis.parse(synth([(0, press)]))
        res = analysis.analyze(log, device="virtual mouse")
        self.assertEqual(len(res["samples"]), 1)
        s = res["samples"][0]
        resp_t = 1_000_000_000 + 52 * FRAME  # first frame after press is 51; it responds one later
        self.assertAlmostEqual(s["latency_ms"], (resp_t + 10 * MS - press) / 1e6, places=3)
        self.assertEqual(s["method"], "scanout")
        self.assertEqual(s["frames_after"], 1)
        self.assertAlmostEqual(s["display_ms"], 9.5, places=3)

    def test_device_filter(self):
        press = 1_000_000_000 + 50 * FRAME
        log = analysis.parse(synth([(1, press)]))
        self.assertEqual(analysis.analyze(log, device="virtual mouse")["presses"], 0)
        self.assertEqual(analysis.analyze(log)["presses"], 1)

    def test_no_change_is_a_miss(self):
        log = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], respond=False))
        res = analysis.analyze(log)
        self.assertEqual(res["samples"], [])
        self.assertEqual(res["misses"][0]["reason"], "no change seen")

    def test_noisy_baseline_raises_threshold(self):
        # +-20 luma flicker must not count as a response
        log = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], respond=False, noise=[0, 20, 0, 20]))
        self.assertEqual(analysis.analyze(log)["samples"], [])

    def test_no_timing_falls_back_to_gpu_done(self):
        log = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], timing=False))
        s = analysis.analyze(log)["samples"][0]
        self.assertEqual(s["method"], "gpu-done")
        self.assertNotIn("display_ms", s)

    def test_empty_timing_reports_fall_back_to_gpu_done(self):
        text = synth([(0, 1_000_000_000 + 50 * FRAME)], timing=False).replace(" 0 1 928", " 1 1 928")
        log = analysis.parse(text)
        self.assertTrue(log.swapchains[0].timing)
        self.assertEqual(analysis.analyze(log)["samples"][0]["method"], "gpu-done")

    def test_xwayland_vsync_uses_dequeued_time(self):
        log = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], kind="dq", mode=2))
        self.assertEqual(analysis.analyze(log)["samples"][0]["method"], "scanout")

    def test_xwayland_without_vsync_uses_gpu_done(self):
        log = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], kind="dq", mode=1))
        self.assertEqual(analysis.analyze(log)["samples"][0]["method"], "gpu-done")

    def test_dropped_frame_moves_to_next_shown(self):
        press = 1_000_000_000 + 50 * FRAME
        text = synth([(0, press)])
        text = text.replace(f"S 0 52 {1_000_000_000 + 52 * FRAME + 10 * MS} px", "S 0 52 0 drop")
        s = analysis.analyze(analysis.parse(text))["samples"][0]
        self.assertAlmostEqual(s["latency_ms"], (1_000_000_000 + 53 * FRAME + 10 * MS - press) / 1e6, places=3)

    def test_many_presses(self):
        presses = [(0, 1_000_000_000 + k * 60 * FRAME + 1234567) for k in range(1, 6)]
        log = analysis.parse(synth(presses, n=420))
        res = analysis.analyze(log)
        self.assertEqual(len(res["samples"]), 5)
        summ = analysis.summarize(res)
        self.assertEqual(summ["measured"], 5)
        self.assertIn("median", summ)

    def test_truncated_line_is_ignored(self):
        text = synth([(0, 1_000_000_000 + 50 * FRAME)]) + "P 0 99"
        self.assertEqual(len(analysis.analyze(analysis.parse(text))["samples"]), 1)

    def test_pick_main_prefers_nested_gamescope_output(self):
        a = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)]).replace("fakegame", "gamescope-wl"))
        b = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], n=500))
        self.assertIs(analysis.pick_main([a, b]), a)

    def test_pick_main_headless_gamescope_is_skipped(self):
        a = analysis.parse("I 1 1 gamescope-wl\nO 0.1.0 0.5 0.5 64 300\n")
        b = analysis.parse(synth([(0, 1_000_000_000 + 50 * FRAME)], n=100))
        self.assertIs(analysis.pick_main([a, b]), b)


class StatsTest(unittest.TestCase):
    def test_describe(self):
        d = stats.describe([1, 2, 3, 4, 5])
        self.assertEqual(d["median"], 3)
        self.assertEqual(d["p25"], 2)
        self.assertAlmostEqual(d["sd"], 1.581, places=3)

    def test_mann_whitney(self):
        a = [float(x) for x in range(30)]
        _, p_same = stats.mann_whitney(a, a)
        _, p_diff = stats.mann_whitney(a, [x + 40 for x in a])
        self.assertGreater(p_same, 0.9)
        self.assertLess(p_diff, 1e-6)

    def test_bootstrap(self):
        a = [10.0 + (i % 7) * 0.1 for i in range(60)]
        b = [x + 2 for x in a]
        d, lo, hi = stats.bootstrap_median_diff(a, b)
        self.assertAlmostEqual(d, 2.0, places=6)
        self.assertLessEqual(lo, 2.0)
        self.assertGreaterEqual(hi, 2.0)


class ChartTest(unittest.TestCase):
    def test_svgs_are_valid_xml(self):
        lat = [10 + (i % 13) * 0.37 for i in range(100)]
        for theme in ("dark", "light"):
            ET.fromstring(chart.histogram(lat, label="test", theme=theme, context="ctx"))
            ET.fromstring(chart.compare([{"label": "a <b>", "latencies": lat, "note": "n"},
                                         {"label": "b", "latencies": [x + 3 for x in lat]}], theme=theme))


class UinputTest(unittest.TestCase):
    def test_ioctl_numbers(self):
        self.assertEqual(uinput.UI_SET_EVBIT, 0x40045564)
        self.assertEqual(uinput.UI_DEV_SETUP, 0x405C5503)
        self.assertEqual(uinput.UI_DEV_CREATE, 0x5501)
        self.assertEqual(uinput._EVENT.size, 24)


if __name__ == "__main__":
    unittest.main()
