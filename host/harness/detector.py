"""
Growth-rate STFT peak detector — the analysis layer of ground-rules-and-facts.md
section 6.1, and the direct measurement of section 7's thesis ("detect by growth
rate, not by rank").

This is a **streaming, frame-causal** reference implementation: `GrowthDetector`
holds state across calls and only ever looks at the current frame plus what it
remembers from the past, exactly like the real-time analysis task on the Bela will
have to. That is deliberate — it is the point of this module. A detector that peeks
ahead could never be ported to `render.cpp`'s auxiliary task, and its latency numbers
would be fiction. `bela/detector-passthrough/render.cpp` is the real-time C++ port;
this module is what it is checked against and what a takes-based validation (Phase 3's
exit criterion — "when the feedback jumps, how many ms before the growth scorer flags
the new partial") runs against, offline, before ever touching hardware.

Per-bin features, maintained across frames (section 6.1):
  - magnitude (dBFS)
  - growth rate (dB/frame, one-pole smoothed) — the important one
  - narrowness (peak-to-neighbouring-bin prominence, PNPR-style)
  - persistence (consecutive frames a bin has stood as a peak)

A bin **arms** when its smoothed growth rate crosses a threshold while it is a
persistent, prominent peak — not when it becomes the loudest bin (section 6.2). It
**releases** when its magnitude falls and stays below its own armed level minus a
release margin for a hold time.

    from harness.detector import GrowthDetector
    det = GrowthDetector(sample_rate=48000)
    events = det.run_offline(audio)          # causal, one hop at a time internally
    # events: list of dicts, one per arm/release, with frame index, time, freq, etc.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
from scipy.signal import find_peaks, get_window

# ----------------------------------------------------------------- defaults
# Matches ground-rules-and-facts.md 6.1's starting point and 7's latency budget.

WINDOW = 2048
HOP = 256                          # ~5.3 ms at 48 kHz, ~5.8 ms at 44.1 kHz
PROMINENCE_DB = 6.0                 # how far a bin must stand above its shoulders
MIN_PEAK_SEPARATION_BINS = 3
GROWTH_SMOOTHING = 0.6              # one-pole coefficient on the new sample, 0..1
ARM_GROWTH_DB_PER_FRAME = 0.8       # smoothed growth needed to arm -- ~140 dB/s at the
                                     # default hop/sample-rate combination. Comfortably
                                     # above ordinary playing dynamics (a plucked string's
                                     # broadband attack is filtered out by the narrowness/
                                     # persistence gate below, not by this threshold alone)
                                     # and comfortably below the runaway growth ground-
                                     # rules 7 describes (~1000 dB/s at typical loop gain
                                     # margins near unity).
ARM_HOLD_FRAMES = 2                 # consecutive qualifying frames before arming
                                     # (anti-chatter, ground-rules 6.2, cheap version)
RELEASE_MARGIN_DB = 10.0            # release once this far below the armed-time level
RELEASE_HOLD_FRAMES = 8             # ...for this many consecutive frames
SILENCE_DBFS = -80.0                # bins quieter than this never arm


@dataclass
class ArmEvent:
    kind: str            # "arm" or "release"
    frame: int
    time_s: float
    bin_index: int
    freq_hz: float
    magnitude_db: float
    growth_db_per_frame: float

    def to_dict(self) -> dict:
        return {
            "kind": self.kind, "frame": self.frame, "time_s": self.time_s,
            "bin_index": self.bin_index, "freq_hz": self.freq_hz,
            "magnitude_db": self.magnitude_db,
            "growth_db_per_frame": self.growth_db_per_frame,
        }


@dataclass
class _BinState:
    persistence: int = 0
    armed: bool = False
    armed_level_db: float = -200.0
    below_release_frames: int = 0
    qualifying_frames: int = 0   # consecutive frames meeting the arm condition


class GrowthDetector:
    """Streaming per-bin growth-rate arm/release detector.

    Call `push(block)` once per `hop`-length block of audio, in order, with no gaps
    and no repeats — exactly how render.cpp will feed it. `run_offline(x)` is a
    convenience that does this for a whole array, still causally, one hop at a time.
    """

    def __init__(
        self,
        sample_rate: int,
        window: int = WINDOW,
        hop: int = HOP,
        prominence_db: float = PROMINENCE_DB,
        min_peak_separation_bins: int = MIN_PEAK_SEPARATION_BINS,
        growth_smoothing: float = GROWTH_SMOOTHING,
        arm_growth_db_per_frame: float = ARM_GROWTH_DB_PER_FRAME,
        arm_hold_frames: int = ARM_HOLD_FRAMES,
        release_margin_db: float = RELEASE_MARGIN_DB,
        release_hold_frames: int = RELEASE_HOLD_FRAMES,
        silence_dbfs: float = SILENCE_DBFS,
    ) -> None:
        if hop > window:
            raise ValueError("hop cannot exceed window")
        self.sample_rate = sample_rate
        self.window = window
        self.hop = hop
        self.prominence_db = prominence_db
        self.min_peak_separation_bins = min_peak_separation_bins
        self.growth_smoothing = growth_smoothing
        self.arm_growth_db_per_frame = arm_growth_db_per_frame
        self.arm_hold_frames = arm_hold_frames
        self.release_margin_db = release_margin_db
        self.release_hold_frames = release_hold_frames
        self.silence_dbfs = silence_dbfs

        self._win = get_window("hann", window, fftbins=True).astype(np.float64)
        self._ring = np.zeros(window, dtype=np.float64)  # last `window` samples seen
        self._filled = 0        # samples pushed in so far, capped at window
        self._n_bins = window // 2 + 1
        self._prev_mag_db = np.full(self._n_bins, -200.0)
        self._smoothed_growth = np.zeros(self._n_bins)
        self._bins = [_BinState() for _ in range(self._n_bins)]
        self._frame_idx = 0

    # -------------------------------------------------------------- internals

    def _spectrum_db(self) -> np.ndarray:
        windowed = self._ring * self._win
        spec = np.fft.rfft(windowed)
        mag = np.abs(spec) / (self.window / 2.0)   # ~full-scale for a windowed sinusoid
        return 20.0 * np.log10(np.maximum(mag, 1e-12))

    def _process_frame(self, mag_db: np.ndarray) -> list[ArmEvent]:
        events: list[ArmEvent] = []
        t = self._frame_idx * self.hop / float(self.sample_rate)

        growth = mag_db - self._prev_mag_db
        self._smoothed_growth = (
            self.growth_smoothing * growth
            + (1.0 - self.growth_smoothing) * self._smoothed_growth
        )

        peak_idx, _ = find_peaks(
            mag_db, prominence=self.prominence_db, distance=self.min_peak_separation_bins,
        )
        is_peak = np.zeros(self._n_bins, dtype=bool)
        is_peak[peak_idx] = True

        for i in range(self._n_bins):
            st = self._bins[i]
            db = mag_db[i]

            if is_peak[i]:
                st.persistence += 1
            else:
                st.persistence = 0

            qualifies = (
                is_peak[i]
                and db > self.silence_dbfs
                and self._smoothed_growth[i] >= self.arm_growth_db_per_frame
            )
            st.qualifying_frames = st.qualifying_frames + 1 if qualifies else 0

            if not st.armed and st.qualifying_frames >= self.arm_hold_frames:
                st.armed = True
                st.armed_level_db = db
                st.below_release_frames = 0
                events.append(ArmEvent(
                    "arm", self._frame_idx, t, i,
                    i * self.sample_rate / self.window, db, self._smoothed_growth[i],
                ))
            elif st.armed:
                st.armed_level_db = max(st.armed_level_db, db)
                if db <= st.armed_level_db - self.release_margin_db:
                    st.below_release_frames += 1
                else:
                    st.below_release_frames = 0
                if st.below_release_frames >= self.release_hold_frames:
                    st.armed = False
                    st.qualifying_frames = 0
                    events.append(ArmEvent(
                        "release", self._frame_idx, t, i,
                        i * self.sample_rate / self.window, db, self._smoothed_growth[i],
                    ))

        self._prev_mag_db = mag_db
        self._frame_idx += 1
        return events

    # --------------------------------------------------------------- public

    def push(self, block: np.ndarray) -> list[ArmEvent]:
        """Feed one hop-length block of new audio samples. Returns new events, if any."""
        block = np.asarray(block, dtype=np.float64)
        if block.size != self.hop:
            raise ValueError(f"push() expects exactly {self.hop} samples, got {block.size}")
        self._ring = np.concatenate([self._ring[self.hop:], block])
        self._filled = min(self._filled + block.size, self.window)
        if self._filled < self.window:
            self._frame_idx += 1  # keep time base correct even before the first full frame
            return []
        return self._process_frame(self._spectrum_db())

    def run_offline(self, x: np.ndarray) -> list[ArmEvent]:
        """Push a whole signal through hop by hop, causally. Trailing partial hop dropped."""
        x = np.asarray(x, dtype=np.float64)
        n_hops = x.size // self.hop
        events: list[ArmEvent] = []
        for h in range(n_hops):
            events.extend(self.push(x[h * self.hop:(h + 1) * self.hop]))
        return events


if __name__ == "__main__":
    import json
    import sys

    import soundfile as sf

    for path in sys.argv[1:]:
        data, sr = sf.read(path, always_2d=False)
        x = data[:, 0] if getattr(data, "ndim", 1) > 1 else data
        det = GrowthDetector(sample_rate=sr)
        evs = det.run_offline(x)
        print(path)
        for e in evs:
            print(json.dumps(e.to_dict()))
