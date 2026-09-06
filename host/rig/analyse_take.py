"""
Score a recorded take from the feedback rig, offline.

`rig.loop` writes stereo WAVs where channel 0 is the rig input (the pickup) and channel 1
is the DSP output — see rig/loop.py's `_write_wav`. This tool reads one of those back and
turns it into numbers, so a tuning change can be judged by a diff of two JSON blobs instead
of by memory of how the last take sounded. It is the M1 sibling of `harness.metrics`: that
module scores the DSP's own idea of what happened, in-process; this one scores whatever
actually got recorded, after the fact, from disk.

No soundfile dependency — WAVs are read with the stdlib `wave` module, same as `rig.loop`
writes them.

    cd host && python3 -m rig.analyse_take logs/2026-09-06/take_003.wav
    cd host && python3 -m rig.analyse_take take.wav --channel 1 --json

Offline only. Nothing here opens an audio stream.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import wave
from dataclasses import asdict, dataclass, field

import numpy as np
from scipy.signal import find_peaks, stft

# ----------------------------------------------------------------- defaults

WINDOW = 4096          # long, because adjacent string partials must be resolvable.
HOP = 512              # this is offline: no realtime budget to respect.
PARTIAL_FLOOR_DB = 30.0    # a partial counts if it is within this of the loudest
PEAK_PROMINENCE_DB = 6.0   # how far a bin must stand above its neighbours to count
PEAK_MIN_SEPARATION_BINS = 3
SILENCE_DBFS = -60.0       # a frame quieter than this is not feeding back
DOMINANCE_CAP_DB = 120.0   # reported instead of infinity when one partial stands alone
ONSET_MIN_S = 0.5          # how long "sustained" has to hold before it counts as onset
TOP_PARTIALS_N = 5
TOP_PARTIAL_BIN_CENTS = 25.0  # cluster width when ranking "the" strongest partials

_EPS = 1e-12
_NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


@dataclass
class Partial:
    freq_hz: float
    level_db: float   # relative to the loudest partial in that frame, so <= 0


@dataclass
class TakeAnalysis:
    path: str
    duration_s: float
    sample_rate: int
    dominant_hz: float                 # median freq of the loudest partial across frames
    dominant_stability_cents: float    # IQR of the dominant partial's frequency, in cents
    mean_partials: float               # mean count of partials within 30 dB of the loudest
    p10_partials: float                # 10th percentile of that count - the bad moments
    dominance_db: float                # loudest vs sum of the rest, mean. LOWER is better
    top_partials: list = field(default_factory=list)   # strongest few Partial entries
    feedback_onset_s: float | None = None   # when sustained tone starts, None if never
    sustained_fraction: float = 0.0    # fraction of frames above the silence floor

    def to_dict(self) -> dict:
        return asdict(self)


# ------------------------------------------------------------------ WAV I/O

def load_wav(path) -> tuple[np.ndarray, int]:
    """Read a WAV via the stdlib. Returns (samples [n, channels] float32 in -1..1, sr)."""
    with wave.open(str(path), "rb") as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        sr = w.getframerate()
        n_frames = w.getnframes()
        raw = w.readframes(n_frames)

    if sampwidth == 1:
        # 8-bit WAV PCM is the odd one out: unsigned, centred on 128.
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif sampwidth == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif sampwidth == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        as_i32 = (
            b[:, 0].astype(np.int32)
            | (b[:, 1].astype(np.int32) << 8)
            | (b[:, 2].astype(np.int32) << 16)
        )
        as_i32 = np.where(as_i32 & 0x800000, as_i32 - 0x1000000, as_i32)
        data = as_i32.astype(np.float32) / 8388608.0
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"unsupported WAV sample width: {sampwidth} bytes")

    data = data.reshape(-1, max(n_channels, 1))
    return data, sr


# ------------------------------------------------------------------ helpers
# (peak picking and interpolation mirror harness.metrics; window/hop differ, and this
# module is offline-only, so it stays a self-contained sibling rather than an import.)

def _peaks_in_frame(mag_db: np.ndarray, prominence_db: float,
                    min_separation_bins: int = PEAK_MIN_SEPARATION_BINS) -> np.ndarray:
    if mag_db.size < 3:
        return np.empty(0, dtype=int)
    idx, _ = find_peaks(mag_db, prominence=prominence_db, distance=min_separation_bins)
    return idx


def _interpolate_peak(mag_db: np.ndarray, k: int) -> float:
    """Quadratic interpolation around bin k, on the log-magnitude spectrum.

    Without this the dominant frequency quantises to bin centres and a stability
    measurement in cents becomes meaningless — the IQR would just measure the bin grid.
    """
    if k <= 0 or k >= mag_db.size - 1:
        return float(k)
    a, b, c = mag_db[k - 1], mag_db[k], mag_db[k + 1]
    denom = a - 2.0 * b + c
    if abs(denom) < 1e-12:
        return float(k)
    return float(k) + 0.5 * (a - c) / denom


def _feedback_onset(voiced: np.ndarray, hop: int, sr: int,
                     min_s: float = ONSET_MIN_S) -> float | None:
    """First time `voiced` stays True for at least `min_s` seconds continuously."""
    min_frames = max(1, math.ceil(min_s * sr / hop))
    run = 0
    for i, v in enumerate(voiced):
        if v:
            run += 1
            if run >= min_frames:
                return float((i - min_frames + 1) * hop / sr)
        else:
            run = 0
    return None


def _top_partials(freqs: list, levels_db: list, abs_dbfs: list,
                   n: int = TOP_PARTIALS_N,
                   bin_cents: float = TOP_PARTIAL_BIN_CENTS) -> list[Partial]:
    """Cluster every detected peak instance by frequency and rank by total energy.

    A single strong partial that recurs across many frames should outrank a partial
    that was merely loud once, so groups are ranked by summed linear power, not by a
    single frame's dB reading.
    """
    if not freqs:
        return []
    freqs_a = np.asarray(freqs, dtype=np.float64)
    keys = np.round(1200.0 * np.log2(freqs_a) / bin_cents).astype(np.int64)
    power = 10.0 ** (np.asarray(abs_dbfs, dtype=np.float64) / 10.0)

    groups: dict[int, dict] = {}
    for key, f, lvl, p in zip(keys, freqs_a, levels_db, power):
        g = groups.setdefault(int(key), {"freqs": [], "levels": [], "power_sum": 0.0})
        g["freqs"].append(f)
        g["levels"].append(lvl)
        g["power_sum"] += float(p)

    ranked = sorted(groups.values(), key=lambda g: g["power_sum"], reverse=True)
    return [
        Partial(freq_hz=float(np.mean(g["freqs"])), level_db=float(np.mean(g["levels"])))
        for g in ranked[:n]
    ]


def hz_to_note(hz: float) -> tuple[str, float]:
    """Nearest note name (A4 = 440 Hz) and the deviation in cents. ("B3", -0.4)"""
    if hz <= 0.0 or not math.isfinite(hz):
        return "-", 0.0
    midi = 69.0 + 12.0 * math.log2(hz / 440.0)
    nearest = round(midi)
    cents = (midi - nearest) * 100.0
    name = _NOTE_NAMES[nearest % 12]
    octave = nearest // 12 - 1
    return f"{name}{octave}", cents


# ------------------------------------------------------------------- public

def analyse_take(path, channel: int = 0) -> TakeAnalysis:
    data, sr = load_wav(path)
    if channel >= data.shape[1]:
        raise ValueError(f"channel {channel} not in {data.shape[1]}-channel file {path}")
    x = data[:, channel].astype(np.float64)
    duration_s = x.size / float(sr) if sr else 0.0

    if x.size < WINDOW:
        # Too short for even one analysis window: report what we can, nothing crashes.
        return TakeAnalysis(
            path=str(path), duration_s=duration_s, sample_rate=sr,
            dominant_hz=0.0, dominant_stability_cents=0.0,
            mean_partials=0.0, p10_partials=0.0, dominance_db=DOMINANCE_CAP_DB,
            top_partials=[], feedback_onset_s=None, sustained_fraction=0.0,
        )

    freqs_axis, _, Z = stft(
        x, fs=sr, nperseg=WINDOW, noverlap=WINDOW - HOP,
        window="hann", boundary=None, padded=False,
    )
    mag = np.abs(Z)                                    # (bins, frames)
    n_frames = mag.shape[1]
    # scipy's default STFT scaling puts a stationary sinusoid of amplitude A at
    # |Z| = A/2 regardless of window/hop, so the factor of two recovers full scale
    # (same convention as harness.metrics — see its comment for the derivation).
    mag_dbfs = 20.0 * np.log10(np.maximum(2.0 * mag, _EPS))

    voiced = np.zeros(n_frames, dtype=bool)
    for i in range(n_frames):
        seg = x[i * HOP: i * HOP + WINDOW]
        rms = math.sqrt(float(np.dot(seg, seg)) / seg.size) if seg.size else 0.0
        rms_dbfs = 20.0 * math.log10(max(rms, _EPS))
        voiced[i] = rms_dbfs >= SILENCE_DBFS

    counts: list[int] = []
    dominance: list[float] = []
    dominant_freqs: list[float] = []
    all_freqs: list[float] = []
    all_levels_db: list[float] = []
    all_abs_dbfs: list[float] = []

    for i in range(n_frames):
        if not voiced[i]:
            continue
        frame_db = mag_dbfs[:, i]
        idx = _peaks_in_frame(frame_db, PEAK_PROMINENCE_DB)
        if idx.size == 0:
            counts.append(0)
            continue

        peak_db = frame_db[idx]
        loudest = peak_db.max()
        kept = peak_db >= loudest - PARTIAL_FLOOR_DB
        counts.append(int(kept.sum()))

        power = 10.0 ** (peak_db[kept] / 10.0)
        top = power.max()
        rest = power.sum() - top
        dominance.append(DOMINANCE_CAP_DB if rest <= _EPS
                         else min(10.0 * math.log10(top / rest), DOMINANCE_CAP_DB))

        winner_local = int(np.argmax(peak_db))
        winner_bin = int(idx[winner_local])
        winner_hz = _interpolate_peak(frame_db, winner_bin) * sr / WINDOW
        dominant_freqs.append(winner_hz)

        for j in np.flatnonzero(kept):
            bin_k = int(idx[j])
            hz = _interpolate_peak(frame_db, bin_k) * sr / WINDOW
            all_freqs.append(hz)
            all_levels_db.append(float(peak_db[j] - loudest))
            all_abs_dbfs.append(float(peak_db[j]))

    if dominant_freqs:
        dominant_hz = float(np.median(dominant_freqs))
        cents = 1200.0 * np.log2(np.asarray(dominant_freqs))
        dominant_stability_cents = float(
            np.percentile(cents, 75) - np.percentile(cents, 25)
        )
    else:
        dominant_hz = 0.0
        dominant_stability_cents = 0.0

    return TakeAnalysis(
        path=str(path),
        duration_s=duration_s,
        sample_rate=sr,
        dominant_hz=dominant_hz,
        dominant_stability_cents=dominant_stability_cents,
        mean_partials=float(np.mean(counts)) if counts else 0.0,
        p10_partials=float(np.percentile(counts, 10)) if counts else 0.0,
        dominance_db=float(np.mean(dominance)) if dominance else DOMINANCE_CAP_DB,
        top_partials=_top_partials(all_freqs, all_levels_db, all_abs_dbfs),
        feedback_onset_s=_feedback_onset(voiced, HOP, sr),
        sustained_fraction=float(voiced.mean()) if n_frames else 0.0,
    )


# ---------------------------------------------------------------------- CLI

def _print_human(a: TakeAnalysis) -> None:
    note, cents = hz_to_note(a.dominant_hz)
    onset = f"{a.feedback_onset_s:.2f}s" if a.feedback_onset_s is not None else "never"
    print(a.path)
    print(f"  duration        {a.duration_s:.1f}s  @ {a.sample_rate} Hz")
    print(f"  dominant        {a.dominant_hz:.1f} Hz  ({note} {cents:+.0f} cents)")
    print(f"  stability       {a.dominant_stability_cents:.1f} cents IQR")
    print(f"  partials        mean={a.mean_partials:.2f}  p10={a.p10_partials:.2f}")
    print(f"  dominance       {a.dominance_db:.1f} dB  (lower is better)")
    print(f"  sustained       {a.sustained_fraction:.2f}   onset={onset}")
    if a.top_partials:
        top = "  ".join(
            f"{p.freq_hz:.1f}Hz({hz_to_note(p.freq_hz)[0]},{p.level_db:+.1f}dB)"
            for p in a.top_partials
        )
        print(f"  top partials    {top}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="rig.analyse_take", description=__doc__.split("\n\n")[0],
    )
    p.add_argument("path", help="WAV to analyse (ch0=rig input, ch1=DSP output)")
    p.add_argument("--channel", type=int, default=0, help="channel to analyse (default 0)")
    p.add_argument("--json", action="store_true", help="emit one JSON result object")
    args = p.parse_args(argv)

    try:
        result = analyse_take(args.path, channel=args.channel)
    except (OSError, wave.Error, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(result.to_dict(), indent=2, default=str))
    else:
        _print_human(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
