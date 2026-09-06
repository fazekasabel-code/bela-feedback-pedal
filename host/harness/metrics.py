"""
Proxy metrics for gen1 feedback-regulator takes.

These are a stand-in for Abel's ears. See docs/ground-rules-and-facts.md section 9.
When his rating of a take disagrees with these numbers, the metric is the bug.

    from harness.metrics import analyse
    print(analyse("logs/2026-09-06/take_003.wav"))

Tested on synthetic signals only. No hardware recording has gone through it yet.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, asdict

import numpy as np
from scipy.signal import find_peaks, stft


# ----------------------------------------------------------------- defaults

WINDOW = 2048          # matches the DSP analysis window (docs 6.1)
HOP = 256              # ~5.8 ms at 44.1 kHz
PARTIAL_FLOOR_DB = -30.0   # a "simultaneous partial" is within this of the loudest
PEAK_PROMINENCE_DB = 6.0   # how far a bin must stand above its neighbours to count
SILENCE_DBFS = -60.0       # a frame quieter than this is not feeding back
DOMINANCE_CAP_DB = 120.0   # reported instead of infinity when one partial stands alone
JUMP_CENTS = 50.0          # movement beyond this is a new event, not a glide


@dataclass
class Metrics:
    """The scoreboard. Higher is better except where noted."""

    partial_count: float          # mean simultaneous partials within PARTIAL_FLOOR_DB
    partial_count_p10: float      # 10th percentile — the bad moments, not the good ones
    dominance_ratio_db: float     # loudest partial vs sum of the rest. LOWER is better.
                                  # capped at DOMINANCE_CAP_DB, never infinite.
    peak_flatness: float          # spectral flatness of the peak set, 0..1.
                                  # Zero by definition when fewer than two partials
                                  # are in play — one partial is not "flat", it is bare.
    sustain_fraction: float       # fraction of frames above the silence floor
    alive_at_end: bool            # still feeding back in the final 10% of the take
    mode_switch_rate_hz: float    # dominant-partial jumps per second. Tune, don't maximise.
    peak_sample: float            # max |sample|, for the output-ceiling check
    duration_s: float
    frames_analysed: int

    def to_dict(self) -> dict:
        return asdict(self)

    def __str__(self) -> str:
        return json.dumps(self.to_dict(), indent=2, default=str)


# ------------------------------------------------------------------ helpers

def _to_mono(x: np.ndarray) -> np.ndarray:
    x = np.asarray(x, dtype=np.float64)
    return x.mean(axis=1) if x.ndim == 2 else x


def _peaks_in_frame(mag_db: np.ndarray, prominence_db: float,
                    min_separation_bins: int = 3) -> np.ndarray:
    """Indices of spectral peaks that stand `prominence_db` above their surrounding valleys.

    Prominence, not a bare three-point test: a windowed sinusoid's main lobe spans
    several bins, so its immediate neighbours are only a decibel or two down and a
    neighbour comparison finds nothing. `min_separation_bins` keeps one partial from
    being counted twice across its own main lobe.
    """
    if mag_db.size < 3:
        return np.empty(0, dtype=int)
    idx, _ = find_peaks(mag_db, prominence=prominence_db, distance=min_separation_bins)
    return idx


def _interpolate_peak(mag_db: np.ndarray, k: int) -> float:
    """Quadratic interpolation around bin k. Returns a fractional bin index."""
    if k <= 0 or k >= mag_db.size - 1:
        return float(k)
    a, b, c = mag_db[k - 1], mag_db[k], mag_db[k + 1]
    denom = a - 2.0 * b + c
    if abs(denom) < 1e-12:
        return float(k)
    return float(k) + 0.5 * (a - c) / denom


def _cents(f1: float, f2: float) -> float:
    if f1 <= 0.0 or f2 <= 0.0:
        return math.inf
    return abs(1200.0 * math.log2(f2 / f1))


# ------------------------------------------------------------------- public

def analyse(
    audio,
    sample_rate: int | None = None,
    *,
    window: int = WINDOW,
    hop: int = HOP,
    partial_floor_db: float = PARTIAL_FLOOR_DB,
    prominence_db: float = PEAK_PROMINENCE_DB,
    silence_dbfs: float = SILENCE_DBFS,
) -> Metrics:
    """Score one take.

    `audio` is a path to a soundfile-readable file, or an array plus `sample_rate`.
    """
    if isinstance(audio, str):
        import soundfile as sf
        data, sample_rate = sf.read(audio, always_2d=False)
    else:
        data = audio
        if sample_rate is None:
            raise ValueError("sample_rate is required when passing an array")

    x = _to_mono(data)
    if x.size == 0:
        raise ValueError("empty audio")

    peak_sample = float(np.max(np.abs(x)))
    duration_s = x.size / float(sample_rate)

    freqs, _, Z = stft(
        x,
        fs=sample_rate,
        nperseg=window,
        noverlap=window - hop,
        window="hann",
        boundary=None,
        padded=False,
    )
    mag = np.abs(Z)                                   # (bins, frames)
    n_frames = mag.shape[1]
    if n_frames == 0:
        raise ValueError("audio shorter than one analysis window")

    eps = 1e-12
    # dBFS, not dB relative to the take's own maximum: "is this still feeding back"
    # is an absolute question. scipy's default scaling puts a sinusoid of amplitude A
    # at |Z| = A/2, so the factor of two recovers full scale.
    mag_dbfs = 20.0 * np.log10(np.maximum(2.0 * mag, eps))

    counts: list[int] = []
    dominance: list[float] = []
    flatness: list[float] = []
    dominant_hz: list[float] = []
    voiced = np.zeros(n_frames, dtype=bool)

    for i in range(n_frames):
        frame_db = mag_dbfs[:, i]
        if frame_db.max() < silence_dbfs:
            continue
        voiced[i] = True

        idx = _peaks_in_frame(frame_db, prominence_db)
        if idx.size == 0:
            counts.append(0)
            continue

        peak_db = frame_db[idx]
        loudest = peak_db.max()

        # how many partials are in play at once
        kept = peak_db >= loudest + partial_floor_db
        counts.append(int(kept.sum()))

        # winner-takes-all, measured directly: loudest against the sum of the rest
        power = 10.0 ** (peak_db[kept] / 10.0)
        top = power.max()
        rest = power.sum() - top
        if rest <= eps:
            dominance.append(DOMINANCE_CAP_DB)
        else:
            dominance.append(min(10.0 * math.log10(top / rest), DOMINANCE_CAP_DB))

        # richness of the peak set: geometric vs arithmetic mean. A single partial
        # is trivially "flat" by the formula, which is the opposite of what we mean.
        if power.size < 2:
            flatness.append(0.0)
        else:
            p = np.maximum(power, eps)
            flatness.append(float(np.exp(np.log(p).mean()) / p.mean()))

        # where the winner currently sits, for the switch-rate count
        k = int(idx[int(np.argmax(peak_db))])
        dominant_hz.append(float(_interpolate_peak(frame_db, k) * sample_rate / window))

    if not counts:
        # nothing above the silence floor anywhere
        return Metrics(0.0, 0.0, DOMINANCE_CAP_DB, 0.0, 0.0, False, 0.0,
                       peak_sample, duration_s, n_frames)

    switches = sum(
        1 for a, b in zip(dominant_hz, dominant_hz[1:]) if _cents(a, b) > JUMP_CENTS
    )
    voiced_s = max(int(voiced.sum()) * hop / float(sample_rate), 1e-9)

    tail = voiced[int(0.9 * n_frames):]
    alive_at_end = bool(tail.size and tail.mean() > 0.5)

    return Metrics(
        partial_count=float(np.mean(counts)),
        partial_count_p10=float(np.percentile(counts, 10)),
        dominance_ratio_db=float(np.mean(dominance)) if dominance else DOMINANCE_CAP_DB,
        peak_flatness=float(np.mean(flatness)) if flatness else 0.0,
        sustain_fraction=float(voiced.mean()),
        alive_at_end=alive_at_end,
        mode_switch_rate_hz=switches / voiced_s,
        peak_sample=peak_sample,
        duration_s=duration_s,
        frames_analysed=n_frames,
    )


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        print(path)
        print(analyse(path))
