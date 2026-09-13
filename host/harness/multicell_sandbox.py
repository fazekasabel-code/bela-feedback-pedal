"""
Offline reference simulation of bela/gen1-multicell/render.cpp's N-cell allocator and
actuator, run against synthetic multi-tone signals instead of a real (or simulated)
feedback loop.

WHAT THIS IS NOT: Phase 2's loop simulator (still deliberately skipped, see
phase-plan.md's Status block -- it needs a transfer-function measurement that doesn't
exist yet). This has no feedback, no exciter, no body, no growth over time from a
closed loop. A synthetic tone's amplitude is fixed by the test, not something that can
"bloom" the way a real loop lets an unregulated partial's gain climb to unity. It
CANNOT show whether regulation helps a second partial bloom, because there is no loop
here for anything to bloom in.

WHAT THIS IS: a way to test the allocator/actuator CODE in isolation, open-loop, so a
question like "does this correctly bind to N simultaneous tones and pull each toward
its target level" has an answer that does not depend on a guitar, an exciter, or
today's mount. Built 2026-09-13 after two real-loop gen1-multicell takes read closer to
single-partial than gen1-cell's pass, to separate "is the allocator/actuator code
behaving sanely" from "is this the physical rig" -- see render.cpp's header for the
real-loop findings that prompted this.

Mirrors render.cpp's constants and structure (STFT window/hop, prominence-based
candidate detection, bind-by-rank, +-2 bin glide, release-with-ramp, anti-chatter hold,
lockout, steal-least-active) but is NOT a byte-for-byte port:
  - Candidate detection uses scipy.signal.find_peaks(prominence=...) rather than the
    real-time port's cheap local-shoulder proxy (see detector-passthrough's header for
    why that proxy exists and needs a different threshold) -- fine for testing the
    allocator's decisions given a candidate list, which is this module's actual target.
  - The actuator biquads use the standard Audio EQ Cookbook peaking/bandpass formulas
    (the credited source of Bela's own Biquad.h -- see that file's header comment,
    "based on... earlevel.com"), not Bela's compiled Biquad class itself.
  - The envelope follower is a plain asymmetric one-pole peak follower (fast attack,
    slow release coefficients from the same attack_ms/release_ms constants), not a
    port of Bela's EnvelopeDetector class internals.

    from harness.multicell_sandbox import run_scenario, SINE_SCENARIOS
    result = run_scenario(SINE_SCENARIOS["four_equal_tones"])
    print(result.describe())
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
from scipy.signal import find_peaks

# ----------------------------------------------------------------- constants
# Mirrors bela/gen1-multicell/render.cpp -- keep these two in sync by hand; there is
# no shared source for them yet (same caveat host/rig/gen1_multicell_run.py's
# CELL_PARAMS already carries for the log-record copy of these).

SAMPLE_RATE = 44100
NUM_CELLS = 4
FFT_WINDOW = 2048
HOP = 256
PROMINENCE_DB = 25.0
SILENCE_DBFS = -80.0
STABLE_HOLD_FRAMES = 2
GLIDE_BIN_RADIUS = 2
EXCLUSION_BIN_RADIUS = 6
MIN_BOUND_HOLD_FRAMES = 40
LOCKOUT_FRAMES = 20
RELEASE_MARGIN_DB = 10.0
RELEASE_HOLD_FRAMES = 8

TARGET_DB = -24.0
CELL_Q = 10.0
ATTACK_MS = 3.0
RELEASE_MS = 400.0
MAX_CUT_DB = 30.0
FREQ_LAG_S = 0.02
MIN_CELL_FREQ_HZ = 55.0
MAX_CELL_FREQ_HZ = 2000.0

# NOT in render.cpp yet -- see this module's find_scenario diagnosis below for why it's
# proposed here. 0 reproduces render.cpp's current unconditional-steal behaviour.
STEAL_MARGIN_DB = 0.0


# ------------------------------------------------------------- biquad math
# Audio EQ Cookbook formulas (Robert Bristow-Johnson), the credited source of Bela's
# own Biquad.h ("based on... earlevel.com", which implements the same cookbook).

def _peaking_coeffs(freq_hz: float, fs: float, q: float, gain_db: float):
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * freq_hz / fs
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    b0 = 1.0 + alpha * A
    b1 = -2.0 * cos_w0
    b2 = 1.0 - alpha * A
    a0 = 1.0 + alpha / A
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha / A
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _bandpass_coeffs(freq_hz: float, fs: float, q: float):
    # Constant 0 dB peak gain variant.
    w0 = 2.0 * math.pi * freq_hz / fs
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    b0 = alpha
    b1 = 0.0
    b2 = -alpha
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


class _Biquad:
    """Direct Form I, coefficients recomputed on demand -- mirrors calling
    setFc()/setPeakGain() every sample, the finer of ground-rules 6.3's two allowed
    coefficient-interpolation options and what render.cpp actually does."""

    def __init__(self):
        self.z1 = 0.0
        self.z2 = 0.0

    def process_peaking(self, x: float, freq_hz: float, fs: float, q: float, gain_db: float) -> float:
        b0, b1, b2, a1, a2 = _peaking_coeffs(freq_hz, fs, q, gain_db)
        y = x * b0 + self.z1
        self.z1 = x * b1 + self.z2 - a1 * y
        self.z2 = x * b2 - a2 * y
        return y

    def process_bandpass(self, x: float, freq_hz: float, fs: float, q: float) -> float:
        b0, b1, b2, a1, a2 = _bandpass_coeffs(freq_hz, fs, q)
        y = x * b0 + self.z1
        self.z1 = x * b1 + self.z2 - a1 * y
        self.z2 = x * b2 - a2 * y
        return y


class _EnvelopeFollower:
    """Asymmetric one-pole peak follower -- attack/release coefficients from the
    corresponding _ms constants. A stand-in for EnvelopeDetector(ANALOG, BRANCHING,
    PEAK), not a port of it."""

    def __init__(self, attack_ms: float, release_ms: float, fs: float):
        self.attack_coeff = 1.0 - math.exp(-1.0 / (0.001 * attack_ms * fs))
        self.release_coeff = 1.0 - math.exp(-1.0 / (0.001 * release_ms * fs))
        self.env = 0.0

    def process(self, x: float) -> float:
        target = abs(x)
        coeff = self.attack_coeff if target > self.env else self.release_coeff
        self.env += (target - self.env) * coeff
        return self.env


# ------------------------------------------------------------------- cells

@dataclass
class _Cell:
    bound: bool = False
    bound_bin: int = -1
    bound_frames: int = 0
    bound_peak_mag_db: float = -200.0
    below_release_frames: int = 0
    candidate_freq_hz: float = 220.0
    freq_slewed: float = 220.0
    detect_bpf: _Biquad = field(default_factory=_Biquad)
    actuator_eq: _Biquad = field(default_factory=_Biquad)
    envelope: _EnvelopeFollower = field(
        default_factory=lambda: _EnvelopeFollower(ATTACK_MS, RELEASE_MS, SAMPLE_RATE))
    cut_db_last: float = 0.0


@dataclass
class ScenarioResult:
    name: str
    tones_hz: list
    tone_levels_dbfs: list
    bind_events: int
    release_events: int
    steal_events: int
    final_cut_db_by_tone: dict   # tone_hz -> cut_db applied near the end, or None if unbound
    final_output_level_dbfs_by_tone: dict

    def describe(self) -> str:
        lines = [f"scenario: {self.name}",
                 f"  bind={self.bind_events} release={self.release_events} steal={self.steal_events}"]
        for hz in self.tones_hz:
            cut = self.final_cut_db_by_tone.get(hz)
            out_db = self.final_output_level_dbfs_by_tone.get(hz)
            in_db = self.tone_levels_dbfs[self.tones_hz.index(hz)]
            cut_str = f"{cut:+.1f}dB cut" if cut is not None else "UNBOUND (no cut)"
            lines.append(f"  {hz:7.1f} Hz: in={in_db:+.1f}dBFS  {cut_str}  "
                         f"out~={out_db:+.1f}dBFS (target {TARGET_DB:.0f})")
        return "\n".join(lines)


def _interpolate_peak(mag_db: np.ndarray, k: int) -> float:
    if k <= 0 or k >= mag_db.size - 1:
        return float(k)
    a, b, c = mag_db[k - 1], mag_db[k], mag_db[k + 1]
    denom = a - 2.0 * b + c
    if abs(denom) < 1e-12:
        return float(k)
    p = 0.5 * (a - c) / denom
    return float(k) + max(-0.5, min(0.5, p))


def _bin_to_hz(bin_idx: float) -> float:
    return bin_idx * SAMPLE_RATE / FFT_WINDOW


def run_allocator(x: np.ndarray, steal_margin_db: float = STEAL_MARGIN_DB) -> ScenarioResult:
    """Run the N-cell allocator+actuator over `x` (mono, SAMPLE_RATE). Returns a
    ScenarioResult with per-tone-frequency (nearest bin, for reporting) outcomes --
    the caller supplies which frequencies it cares about via the tones_hz field it
    fills in afterwards; this function itself only needs the audio."""
    n = x.size
    cells = [_Cell() for _ in range(NUM_CELLS)]
    freq_lag_coeff = 1.0 - math.exp(-1.0 / (FREQ_LAG_S * SAMPLE_RATE))

    window = np.hanning(FFT_WINDOW)
    n_bins = FFT_WINDOW // 2 + 1
    persistence = np.zeros(n_bins, dtype=int)
    lockout_bin = [-1] * NUM_CELLS
    lockout_frames = [0] * NUM_CELLS

    bind_events = release_events = steal_events = 0

    y = np.zeros(n)
    samples_since_hop = 0

    for i in range(n):
        xi = x[i]

        # ---- audio-rate cell processing ----------------------------------
        sig = xi
        for c in cells:
            if c.bound:
                c.freq_slewed += (c.candidate_freq_hz - c.freq_slewed) * freq_lag_coeff
            freq_now = min(max(c.freq_slewed, MIN_CELL_FREQ_HZ), MAX_CELL_FREQ_HZ)

            if c.bound:
                partial = c.detect_bpf.process_bandpass(xi, freq_now, SAMPLE_RATE, CELL_Q)
                env = c.envelope.process(partial)
            else:
                env = c.envelope.process(0.0)
            partial_db = 20.0 * math.log10(max(env, 1e-9))
            cut_db = min(max(partial_db - TARGET_DB, 0.0), MAX_CUT_DB)

            sig = c.actuator_eq.process_peaking(sig, freq_now, SAMPLE_RATE, CELL_Q, -cut_db)
            c.cut_db_last = cut_db
        y[i] = sig

        # ---- hop-rate allocator (STFT + bind/glide/release/steal) --------
        samples_since_hop += 1
        if samples_since_hop >= HOP and i + 1 >= FFT_WINDOW:
            samples_since_hop = 0
            frame = x[i + 1 - FFT_WINDOW: i + 1] * window
            mag = np.abs(np.fft.rfft(frame))
            mag_db = 20.0 * np.log10(np.maximum(mag / (FFT_WINDOW / 2.0), 1e-12))

            idx, _ = find_peaks(mag_db, prominence=PROMINENCE_DB, distance=3)
            is_peak = np.zeros(n_bins, dtype=bool)
            is_peak[idx] = True
            persistence[is_peak] += 1
            persistence[~is_peak] = 0

            for ci in range(NUM_CELLS):
                if lockout_frames[ci] > 0:
                    lockout_frames[ci] -= 1

            def occupied(b: int) -> bool:
                for ci, c in enumerate(cells):
                    if c.bound and abs(b - c.bound_bin) <= EXCLUSION_BIN_RADIUS:
                        return True
                    if lockout_frames[ci] > 0 and abs(b - lockout_bin[ci]) <= EXCLUSION_BIN_RADIUS:
                        return True
                return False

            # glide + release for bound cells
            for ci, c in enumerate(cells):
                if not c.bound:
                    continue
                c.bound_frames += 1
                lo = max(4, c.bound_bin - GLIDE_BIN_RADIUS)
                hi = min(n_bins - 5, c.bound_bin + GLIDE_BIN_RADIUS)
                new_bin = max(range(lo, hi + 1), key=lambda b: mag_db[b])
                c.bound_bin = new_bin
                c.candidate_freq_hz = _bin_to_hz(_interpolate_peak(mag_db, new_bin))
                c.bound_peak_mag_db = max(c.bound_peak_mag_db, mag_db[new_bin])
                if c.bound_frames > MIN_BOUND_HOLD_FRAMES:
                    if mag_db[new_bin] <= c.bound_peak_mag_db - RELEASE_MARGIN_DB:
                        c.below_release_frames += 1
                    else:
                        c.below_release_frames = 0
                    if c.below_release_frames >= RELEASE_HOLD_FRAMES:
                        c.bound = False
                        lockout_bin[ci] = c.bound_bin
                        lockout_frames[ci] = LOCKOUT_FRAMES
                        c.bound_bin = -1
                        release_events += 1

            candidates = [
                (b, mag_db[b]) for b in idx
                if mag_db[b] > SILENCE_DBFS and persistence[b] >= STABLE_HOLD_FRAMES
                and not occupied(b)
            ]
            candidates.sort(key=lambda t: -t[1])

            ptr = 0
            for c in cells:
                if c.bound or ptr >= len(candidates):
                    continue
                b, m = candidates[ptr]; ptr += 1
                c.bound = True
                c.bound_bin = b
                c.bound_frames = 0
                c.bound_peak_mag_db = m
                c.below_release_frames = 0
                c.candidate_freq_hz = _bin_to_hz(_interpolate_peak(mag_db, b))
                bind_events += 1

            if ptr < len(candidates):
                steal_target = -1
                lowest_cut = float("inf")
                for ci, c in enumerate(cells):
                    if not c.bound or c.bound_frames <= MIN_BOUND_HOLD_FRAMES:
                        continue
                    if c.cut_db_last < lowest_cut:
                        lowest_cut = c.cut_db_last
                        steal_target = ci
                if steal_target >= 0:
                    b, m = candidates[ptr]
                    incumbent_mag = mag_db[cells[steal_target].bound_bin]
                    if m >= incumbent_mag + steal_margin_db:
                        c = cells[steal_target]
                        lockout_bin[steal_target] = c.bound_bin
                        lockout_frames[steal_target] = LOCKOUT_FRAMES
                        c.bound_bin = b
                        c.bound_frames = 0
                        c.bound_peak_mag_db = m
                        c.below_release_frames = 0
                        c.candidate_freq_hz = _bin_to_hz(_interpolate_peak(mag_db, b))
                        steal_events += 1

    return y, bind_events, release_events, steal_events, cells


def make_tone_mix(freqs_hz: list, levels_dbfs: list, duration_s: float,
                   fs: int = SAMPLE_RATE) -> np.ndarray:
    t = np.arange(int(duration_s * fs)) / fs
    x = np.zeros_like(t)
    for f, db in zip(freqs_hz, levels_dbfs):
        amp = 10.0 ** (db / 20.0)
        x += amp * np.sin(2.0 * math.pi * f * t)
    return x


def run_scenario(name: str, freqs_hz: list, levels_dbfs: list, duration_s: float = 6.0,
                  steal_margin_db: float = STEAL_MARGIN_DB) -> ScenarioResult:
    x = make_tone_mix(freqs_hz, levels_dbfs, duration_s)
    y, binds, releases, steals, cells = run_allocator(x, steal_margin_db=steal_margin_db)

    # Measure each test tone's actual level in the OUTPUT over the final second, via a
    # Goertzel-style narrowband estimate at its exact frequency -- simpler and exact
    # for a known frequency, no STFT bin-quantisation error.
    tail = y[-int(1.0 * SAMPLE_RATE):]
    t_tail = np.arange(tail.size) / SAMPLE_RATE
    final_cut = {}
    final_out_db = {}
    for f in freqs_hz:
        ref_cos = np.cos(2 * math.pi * f * t_tail)
        ref_sin = np.sin(2 * math.pi * f * t_tail)
        amp = 2.0 / tail.size * math.hypot(np.dot(tail, ref_cos), np.dot(tail, ref_sin))
        final_out_db[f] = 20.0 * math.log10(max(amp, 1e-9))

        matched = None
        for c in cells:
            if c.bound_bin >= 0 and abs(_bin_to_hz(c.bound_bin) - f) < (SAMPLE_RATE / FFT_WINDOW) * 3:
                matched = c
                break
        final_cut[f] = matched.cut_db_last if (matched is not None and matched.bound) else None

    return ScenarioResult(
        name=name, tones_hz=freqs_hz, tone_levels_dbfs=levels_dbfs,
        bind_events=binds, release_events=releases, steal_events=steals,
        final_cut_db_by_tone=final_cut, final_output_level_dbfs_by_tone=final_out_db,
    )


SINE_SCENARIOS = {
    "one_loud_tone": dict(freqs_hz=[300.0], levels_dbfs=[-6.0], duration_s=4.0),
    "four_equal_tones": dict(
        freqs_hz=[200.0, 320.0, 470.0, 650.0], levels_dbfs=[-6.0] * 4, duration_s=6.0),
    "five_equal_tones_one_cell_short": dict(
        freqs_hz=[200.0, 320.0, 470.0, 650.0, 820.0], levels_dbfs=[-6.0] * 5, duration_s=6.0),
    "one_loud_three_quiet": dict(
        freqs_hz=[300.0, 500.0, 700.0, 900.0], levels_dbfs=[-6.0, -35.0, -35.0, -35.0],
        duration_s=6.0),
}


if __name__ == "__main__":
    for name, kwargs in SINE_SCENARIOS.items():
        print(run_scenario(name, **kwargs).describe())
        print()
