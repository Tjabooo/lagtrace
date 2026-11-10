"""Small statistics helpers (standard library only)."""

from __future__ import annotations

import math
import random


def percentile(sorted_xs: list[float], p: float) -> float:
    """Linear-interpolated percentile of an already sorted list (p in 0..100)."""
    if not sorted_xs:
        return float("nan")
    if len(sorted_xs) == 1:
        return sorted_xs[0]
    k = (len(sorted_xs) - 1) * p / 100.0
    lo = math.floor(k)
    hi = min(lo + 1, len(sorted_xs) - 1)
    return sorted_xs[lo] + (sorted_xs[hi] - sorted_xs[lo]) * (k - lo)


def median(xs) -> float:
    return percentile(sorted(xs), 50)


def describe(xs: list[float]) -> dict:
    if not xs:
        return {"n": 0}
    s = sorted(xs)
    n = len(s)
    mean = sum(s) / n
    sd = math.sqrt(sum((x - mean) ** 2 for x in s) / (n - 1)) if n > 1 else 0.0
    r = lambda v: round(v, 3)  # noqa: E731
    return {"n": n, "median": r(percentile(s, 50)), "mean": r(mean), "sd": r(sd), "min": r(s[0]), "max": r(s[-1]),
            "p5": r(percentile(s, 5)), "p25": r(percentile(s, 25)), "p75": r(percentile(s, 75)),
            "p95": r(percentile(s, 95)), "p99": r(percentile(s, 99))}


def mann_whitney(a: list[float], b: list[float]) -> tuple[float, float]:
    """Two-sided Mann-Whitney U test (normal approximation with tie correction). Returns (U, p)."""
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return float("nan"), float("nan")
    allv = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    ranks = [0.0] * len(allv)
    ties = 0.0
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[k] = r
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    r1 = sum(r for r, (_, g) in zip(ranks, allv) if g == 0)
    u1 = r1 - n1 * (n1 + 1) / 2
    n = n1 + n2
    mu = n1 * n2 / 2
    var = n1 * n2 / 12 * ((n + 1) - ties / (n * (n - 1)))
    if var <= 0:
        return u1, 1.0
    z = (abs(u1 - mu) - 0.5) / math.sqrt(var)
    p = math.erfc(max(z, 0) / math.sqrt(2))
    return u1, min(1.0, p)


def bootstrap_median_diff(a: list[float], b: list[float], iters: int = 4000, seed: int = 1) -> tuple[float, float, float]:
    """median(b) - median(a) and its 95% bootstrap confidence interval."""
    rng = random.Random(seed)
    diff = median(b) - median(a)
    out = []
    for _ in range(iters):
        ra = [a[rng.randrange(len(a))] for _ in a]
        rb = [b[rng.randrange(len(b))] for _ in b]
        out.append(median(rb) - median(ra))
    out.sort()
    return diff, percentile(out, 2.5), percentile(out, 97.5)
