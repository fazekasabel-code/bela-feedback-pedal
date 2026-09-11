"""Sanity tests for the growth-rate detector, on synthetic signals.

These cannot prove the detector is musically right — only a real jump on the real rig
can (docs/phase-plan.md Phase 3's actual exit criterion). What they prove: the core
claim of ground-rules-and-facts.md section 7 — detecting by growth rate catches a
new mode while it is still far below the incumbent, fast — holds in a controlled
case where the right answer is known exactly, and a stationary signal does not
falsely arm.

    cd host && python3 -m pytest tests/
"""

import numpy as np
import pytest

from harness.detector import GrowthDetector

SR = 44100


def _bin_hz(sr: int, window: int, bin_index: int) -> float:
    return bin_index * sr / window


def test_jump_arms_fast_and_well_below_the_incumbent():
    """A new mode growing at 500 dB/s (loop gain near unity, ground-rules-and-facts.md
    section 7's ballpark) should arm within a handful of frames of its onset -- and
    while still far quieter than the incumbent. That gap between "quiet" and
    "detected" is the entire thesis: rank-based detection could not see this coming
    for a long time yet.
    """
    det = GrowthDetector(sample_rate=SR)
    onset_s = 1.0
    growth_db_per_s = 500.0
    duration_s = 1.3

    t = np.arange(int(duration_s * SR)) / SR
    incumbent = 0.5 * np.sin(2 * np.pi * _bin_hz(SR, det.window, 40) * t)

    challenger_freq = _bin_hz(SR, det.window, 90)
    challenger_db = np.where(t < onset_s, -80.0, -80.0 + growth_db_per_s * (t - onset_s))
    challenger_db = np.minimum(challenger_db, -6.0)  # cap well below the incumbent
    challenger_amp = 10.0 ** (challenger_db / 20.0)
    challenger = challenger_amp * np.sin(2 * np.pi * challenger_freq * t)

    x = incumbent + challenger

    events = det.run_offline(x)
    arms = [e for e in events if e.kind == "arm"]

    challenger_arms = [e for e in arms if abs(e.freq_hz - challenger_freq) < SR / det.window]
    assert challenger_arms, "the growing challenger never armed at all"

    first = challenger_arms[0]
    latency_s = first.time_s - onset_s
    assert 0.0 <= latency_s < 0.05, f"arm latency {latency_s * 1000:.1f} ms is too slow"

    # The whole point: it armed while still quiet, not once it became the loudest bin.
    assert first.magnitude_db < -6.0, (
        f"armed at {first.magnitude_db:.1f} dB -- too close to the incumbent to call "
        "this a growth-rate catch rather than a loudness catch"
    )


def test_stationary_signal_does_not_keep_arming():
    """A steady two-tone signal should arm once near the start (rising from silence
    is real growth) and then settle -- it must not keep re-arming/releasing on a
    signal that never actually changes. That churn would be the false-arming failure
    mode ground-rules 6.2's anti-chatter hold exists to prevent.
    """
    det = GrowthDetector(sample_rate=SR)
    t = np.arange(int(2.0 * SR)) / SR
    x = (
        0.5 * np.sin(2 * np.pi * _bin_hz(SR, det.window, 40) * t)
        + 0.3 * np.sin(2 * np.pi * _bin_hz(SR, det.window, 70) * t)
    )

    events = det.run_offline(x)
    # Ignore the initial onset burst (rising from silence at t=0 is genuine growth);
    # nothing after the first 100 ms should arm or release on an unchanging signal.
    late = [e for e in events if e.time_s > 0.1]
    assert not late, f"spurious events on a stationary signal: {late}"


def test_quiet_noise_floor_does_not_arm():
    """Ordinary background noise, well below the silence floor, must never arm --
    ground rule: no false arming during ordinary playing (or ordinary silence)."""
    rng = np.random.default_rng(0)
    det = GrowthDetector(sample_rate=SR)
    x = rng.normal(scale=10 ** (-70 / 20), size=int(1.0 * SR))

    events = det.run_offline(x)
    assert not [e for e in events if e.kind == "arm"], "noise floor armed a bin"
