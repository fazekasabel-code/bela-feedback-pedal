"""
A minimal CLOSED-LOOP simulator of the electro-acoustic feedback path -- the thing
multicell_sandbox.py explicitly does NOT do (see that module's header: "no feedback, no
exciter, no body, no growth over time from a closed loop"). This module closes the loop:
DSP output feeds a plant model, the plant model feeds back into the DSP input, round trip
after round trip, the way the real rig's controlled loop does (ground-rules-and-facts.md
sec 2, "pickup -> Bela in -> DSP -> Bela out -> power amp -> exciter -> body -> strings ->
pickup").

============================================================================
THE PLANT MODEL BELOW IS DELIBERATELY CRUDE. IT IS NOT A CALIBRATED MODEL OF
THIS RIG. Say it again: it is a stand-in, not a measurement.
============================================================================

The real exciter -> body -> pickup transfer function has never been measured on this rig
(phase-plan.md Phase 1 -- swept-sine measurement -- has not happened). Every number below
(mode frequencies, the open-loop gain per mode, the round-trip delay, the saturation
threshold) is chosen to be *illustrative and qualitatively plausible*, not fitted to
anything real. Nothing here should be read as "what gain Abel should use" or "how loud the
exciter should be driven." Its only legitimate use is qualitative: does a given control law
let N partials sustain together, and is the closed loop stable while it does -- not "is
this rig's true dominance ratio 8 dB."

HOW EACH MODE IS SIMULATED, AND WHY NOT AS A LITERAL BIQUAD-IN-A-DELAY-LOOP
----------------------------------------------------------------------------
The obvious first design -- route a shared delayed signal through a resonant bandpass
biquad per mode and sum the results back into the loop -- was tried while writing this
module and is WRONG, not just crude: a fixed round-trip delay D imposes a round-trip phase
of 2*pi*f*D/fs at frequency f, and unless that phase happens to land on a multiple of 2*pi
for a given mode's exact frequency, the fed-back signal destructively interferes with the
resonator every round trip. Checked numerically for this module's first mode (196 Hz) at a
4 ms / 176-sample delay: round-trip phase is ~1.37 rad away from the nearest multiple of
2*pi -- so that mode DECAYED every round trip despite g_m = 1.12 > 1, because phase, not
g_m, was governing the outcome. That is a real acoustic phenomenon (it is literally why a
comb filter has notches), but it is not the phenomenon this module exists to study, and
letting it dominate silently would have made every number below meaningless.

So each mode is instead simulated directly as a rotating phasor (a lossless per-sample
rotation at its own frequency -- still exactly a discrete-time second-order resonator,
just realised as sample-accurate polar rotation instead of a filter that requires
delay/frequency phase-lock to behave) whose AMPLITUDE is governed by an explicit
continuous-time growth-rate model:

    d(ln A_m)/dt = ln(g_m * actuator_gain_lin_m(t) * saturation_gain(t)) / round_trip_delay_s

integrated with a simple Euler step every sample. Read it as: "if g_m, the cell's current
actuator gain, and the saturation's current compression were all held fixed, one full
round trip (round_trip_delay_s later) would multiply this mode's amplitude by their
product" -- exactly what "open-loop round-trip gain g_m" is defined to mean in the task
this module was built for -- smeared evenly across the round trip's samples so it can be
simulated at audio rate and interact smoothly with the envelope follower and the shared
saturation. This is still a crude approximation (real electro-acoustic growth is not
perfectly exponential moment to moment, and this throws away every cross-mode
intermodulation effect a literal nonlinearity would produce) but it is an HONEST one: it
does exactly, only, and transparently what its name says, with no hidden phase artifact.

THE SHARED SATURATION (the loop's first nonlinearity, ground-rules-and-facts.md sec 5):
a single soft-knee compression factor is computed each sample from the TOTAL instantaneous
level across all modes (L = sqrt(sum of each mode's amplitude squared)):

    saturation_gain = tanh(L / SATURATION_LIMIT) / (L / SATURATION_LIMIT)   (-> 1 as L -> 0)

and multiplied into every mode's growth rate equally -- the same "one shared nonlinearity
punishes every mode's growth at once" mechanism ground-rules sec 5 names as the reason one
partial wins on the real rig: whichever mode is loudest eats the shared headroom fastest,
which is exactly what makes the peaked g_m distribution below so hard: the winner eats the
shared headroom, and a cut-only cell can hand that headroom back but cannot do more.

THE CONTROLLER mirrors bela/gen1-multicell-live/render.cpp's per-cell structure (detector -> envelope follower ->
gain law -> actuator) but cheats on TWO things deliberately, both explicitly licensed by
the task this module was built for:
  - the ALLOCATOR: each cell is statically bound to one plant mode's exact frequency for
    the whole run, instead of running an STFT/find-peaks allocator (multicell_sandbox.py
    already exercises that in isolation; this module's question is about the control law).
  - the DETECTOR: it reads each mode's own true instantaneous amplitude directly (through
    an attack/release envelope follower, so detector *dynamics* -- part 4's question --
    are still real), rather than approximating it with a bandpass filter on a composite
    signal. Licensed explicitly: "you may cheat and use a simple per-mode detector ...
    rather than a full STFT allocator -- the question here is the CONTROL LAW."

  The cell law is render.cpp's, unchanged and cut-only:

      gain_db = -clamp(partial_db - target_db, 0, max_cut_db)

  (see that file's render(), `cutDb = clampf(partialDb - targetDb, 0.0f, maxCutDb)`
  applied as a negative peaking gain). A cell can hold a partial DOWN to the target; it
  has no way to lift one, so on its own it cannot start a partial that is not already
  sustaining. What it does is stop a winner running away, which per ground-rules 5 is
  what was pushing every other mode below unity.

  The variable this module sweeps is therefore `master_boost_db` -- render.cpp's master
  upward unit, one global gain on the cell chain's output. Being global it multiplies the
  round-trip gain of EVERY mode at once, so it is modelled here exactly that way: an extra
  factor on each mode's total round-trip gain, and on the composite output for the ceiling
  metric. master_boost_db = 0 is the rig exactly as it behaves with the unit at unity.

  Output ceiling clamp at 0.5, unconditional, mirrors render.cpp's kOutputCeiling and
  CLAUDE.md safety rule 1 exactly -- a fixed constant here too, nothing in this module
  raises it. It clamps the reconstructed composite WAVEFORM (for the ceiling-hit metric
  and any future recording use) but, honestly stated, does not itself feed back into mode
  growth -- growth is governed entirely by the log-amplitude model above, which the shared
  saturation already keeps finite. A non-finite sample latches a permanent mute for the
  rest of the run (rule 4: "do not attempt to recover and keep running"); once muted, every
  mode's growth rate is forced to a fixed decay (the loop is physically broken -- nothing
  is reinforcing the modes any more) rather than continuing to evaluate g_m.

USAGE
    from harness.loop_sim import run_baseline, sweep_master_boost, DEFAULT_PEAKED_MODES
    result = run_baseline()
    print(result.describe())
    for r in sweep_master_boost([0, 3, 6, 9, 12, 18]):
        print(r.describe())
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import List, Optional, Sequence

import numpy as np

# ----------------------------------------------------------------- constants
# Controller defaults mirror bela/gen1-multicell-live/render.cpp's kTargetDb / kCellQ /
# kAttackMs / kReleaseMs / kMaxCutDb / kOutputCeiling exactly, so a run at
# master_boost_db=0 is comparable to that file's actual tuning rather than some other
# tuning. cell_q no longer feeds a real filter (see module docstring) but is kept as a
# constant and reported for continuity with render.cpp's tuning surface.
SAMPLE_RATE = 44100
TARGET_DB = -24.0
CELL_Q = 10.0
ATTACK_MS = 3.0
RELEASE_MS = 400.0
MAX_CUT_DB = 30.0
OUTPUT_CEILING = 0.5

# Plant constants -- see the CRUDE PLANT MODEL warning in the module docstring. Nothing
# here is measured.
ROUND_TRIP_DELAY_S = 0.004       # a few ms, lumping the whole physical+digital loop
SATURATION_LIMIT = 0.15          # soft-knee level -- the loop's first (shared) nonlinearity
PLUCK_AMPLITUDE = 0.05           # initial per-mode amplitude at t=0 (the broadband "pluck")
PLUCK_JITTER = 0.3               # +-30% random per-mode variation on the pluck, for realism
DEFAULT_DURATION_S = 4.0
MUTED_DECAY_PER_ROUND_TRIP = 0.5  # once muted, each mode halves per round trip (loop is broken)

# Reporting thresholds -- also illustrative, not measured. SUSTAIN_THRESHOLD_DB sits well
# below TARGET_DB so a mode that never got anywhere near its target still counts as
# "sustained" if it is clearly alive rather than decayed into the noise floor; it is not
# meant to be a tight tolerance around the target.
SUSTAIN_WINDOW_S = 0.2            # trailing window used for "final level" per mode
SUSTAIN_THRESHOLD_DB = -40.0
SETTLE_TOL_DB = 1.5               # "within this many dB of its own final value"
HUNTING_RIPPLE_DB = 1.0           # stdev over the trailing window above this = hunting
DIVERGENCE_CEILING_FRACTION = 0.5   # this much of the run pinned at the ceiling = diverged

_LN_FLOOR = math.log(1e-9)        # amplitude floor, in nats, to keep log-domain math finite
_LN_CEIL = math.log(100.0)        # generous numerical safety ceiling, not a physical limit


class _EnvelopeFollower:
    """Asymmetric one-pole peak follower -- attack/release coefficients from the
    corresponding *_ms constants. Same shape as multicell_sandbox._EnvelopeFollower,
    reimplemented here to keep this file standalone (per the instruction this was written
    under not to touch any existing file)."""

    __slots__ = ("attack_coeff", "release_coeff", "env")

    def __init__(self, attack_ms: float, release_ms: float, fs: float):
        self.attack_coeff = 1.0 - math.exp(-1.0 / (0.001 * attack_ms * fs))
        self.release_coeff = 1.0 - math.exp(-1.0 / (0.001 * release_ms * fs))
        self.env = 0.0

    def process(self, x: float) -> float:
        target = abs(x)
        coeff = self.attack_coeff if target > self.env else self.release_coeff
        self.env += (target - self.env) * coeff
        return self.env


def _clamp(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else (hi if x > hi else x)


def _cell_gain_db(partial_db: float, target_db: float, max_cut_db: float) -> float:
    """render.cpp's cell law: cut a partial above target, do nothing to one below it.

    gain_db = -clamp(partial_db - target_db, 0, max_cut_db), i.e. always <= 0. This is a
    faithful copy of render.cpp's
        cutDb = clampf(partialDb - targetDb, 0.0f, maxCutDb)
        gActuatorEq[c].setPeakGain(-appliedCutDb)
    and deliberately has no boost branch -- a cell here can only ever attenuate, exactly
    as ground-rules 6.3 specifies ("peaking-EQ biquad with negative gain").
    """
    return -_clamp(partial_db - target_db, 0.0, max_cut_db)


# ------------------------------------------------------------------- plant

@dataclass(frozen=True)
class Mode:
    """One plant resonance -- a stand-in for one string/body mode. See the module
    docstring's CRUDE PLANT MODEL warning: freq_hz and g are illustrative, not measured."""
    freq_hz: float
    g: float          # open-loop round-trip linear gain at unity DSP gain


# The peaked distribution this module exists to test: two modes just above unity
# (1.12, 1.04), four spread well below it (0.85 down to 0.25), per the task's suggested
# shape. Frequencies are just spread out for legibility in the report -- with the
# per-mode-exact detector this module uses (see module docstring), spacing does not
# affect detector selectivity the way it would with a real bandpass.
DEFAULT_PEAKED_MODES: tuple = (
    Mode(freq_hz=196.0, g=1.12),
    Mode(freq_hz=246.9, g=1.04),
    Mode(freq_hz=311.1, g=0.85),
    Mode(freq_hz=370.0, g=0.65),
    Mode(freq_hz=440.0, g=0.45),
    Mode(freq_hz=493.9, g=0.25),
)


@dataclass
class ControllerConfig:
    target_db: float = TARGET_DB
    max_cut_db: float = MAX_CUT_DB
    # render.cpp's master upward unit: one global gain, in dB, on the cell chain's
    # output. Global, so it multiplies every mode's round-trip gain equally.
    master_boost_db: float = 0.0
    # WHERE the master unit sits relative to the cells. True = ahead of them, so the
    # detector sees the boosted signal and each bound partial is still regulated to the
    # target; the boost then only has free rein on partials no cell has taken. False =
    # after them, so the cells regulate to a target the master then multiplies and
    # cannot see. See test_master_unit_ahead_of_the_cells_does_not_slam_the_ceiling.
    master_ahead_of_cells: bool = True
    attack_ms: float = ATTACK_MS
    release_ms: float = RELEASE_MS
    output_ceiling: float = OUTPUT_CEILING


# ------------------------------------------------------------------- results

@dataclass
class ModeOutcome:
    freq_hz: float
    g_open_loop: float
    final_partial_db: float
    sustained: bool


@dataclass
class SimResult:
    master_boost_db: float
    duration_s: float
    modes: List[ModeOutcome]
    ceiling_hits: int
    n_samples: int
    settle_time_s: float
    ripple_db: float
    diverged: bool
    nonfinite: bool

    @property
    def n_sustained(self) -> int:
        return sum(1 for m in self.modes if m.sustained)

    @property
    def spread_db(self) -> Optional[float]:
        levels = [m.final_partial_db for m in self.modes if m.sustained]
        if len(levels) < 2:
            return None
        return max(levels) - min(levels)

    @property
    def ceiling_hit_fraction(self) -> float:
        return self.ceiling_hits / self.n_samples if self.n_samples else 0.0

    @property
    def hunting(self) -> bool:
        return self.ripple_db > HUNTING_RIPPLE_DB

    def describe(self) -> str:
        lines = [
            f"master_boost_db={self.master_boost_db:+.1f}  "
            f"sustained={self.n_sustained}/{len(self.modes)}  "
            f"spread={'n/a' if self.spread_db is None else f'{self.spread_db:.1f} dB'}  "
            f"ceiling_hits={self.ceiling_hits} ({100.0 * self.ceiling_hit_fraction:.2f}%)  "
            f"settle={self.settle_time_s:.2f}s  "
            f"ripple={self.ripple_db:.2f} dB  "
            f"{'DIVERGED ' if self.diverged else ''}"
            f"{'NONFINITE ' if self.nonfinite else ''}"
            f"{'HUNTING' if self.hunting else 'settled cleanly'}"
        ]
        for m in self.modes:
            tag = "sustained" if m.sustained else "died out "
            lines.append(f"    {m.freq_hz:7.1f} Hz  g_open_loop={m.g_open_loop:.2f}  "
                          f"final={m.final_partial_db:+7.1f} dB  [{tag}]")
        return "\n".join(lines)


# --------------------------------------------------------------- simulation

def simulate(modes: Sequence[Mode], ctrl: ControllerConfig, duration_s: float = DEFAULT_DURATION_S,
             fs: int = SAMPLE_RATE, round_trip_delay_s: float = ROUND_TRIP_DELAY_S,
             saturation_limit: float = SATURATION_LIMIT, pluck_amplitude: float = PLUCK_AMPLITUDE,
             seed: int = 0) -> SimResult:
    """Run one closed-loop scenario end to end and return a SimResult.

    Per-sample update, matching the module docstring's log-amplitude growth model:
      1. total level L = sqrt(sum of each mode's current amplitude^2)
      2. shared saturation_gain from L (the loop's first, shared nonlinearity)
      3. per mode: envelope-follow its current amplitude -> partial_db -> _cell_gain_db
         -> actuator_gain_lin
      4. per mode: d(ln A)/dt = ln(g_m * actuator_gain_lin * saturation_gain) /
         round_trip_delay_s, Euler-integrated one sample
      5. reconstruct the composite waveform from each mode's amplitude and running phase,
         for the output-ceiling clamp/metric only (see module docstring: this does not
         feed back into growth, which the log-amplitude model + shared saturation already
         keep finite).
    """
    n_modes = len(modes)
    n_samples = int(round(duration_s * fs))
    delay_samples = max(1, int(round(round_trip_delay_s * fs)))

    rng = np.random.default_rng(seed)
    jitter = 1.0 + rng.uniform(-PLUCK_JITTER, PLUCK_JITTER, size=n_modes)
    ln_A = np.log(np.maximum(pluck_amplitude * jitter, 1e-9))

    envelopes = [_EnvelopeFollower(ctrl.attack_ms, ctrl.release_ms, fs) for _ in modes]

    # Per-mode rotation (cos, sin) for the composite waveform -- a lossless phasor rotated
    # once per sample, exact for the mode's own frequency, no filter/phase-lock issues.
    cos_w = np.array([math.cos(2.0 * math.pi * m.freq_hz / fs) for m in modes])
    sin_w = np.array([math.sin(2.0 * math.pi * m.freq_hz / fs) for m in modes])
    ph_c = np.ones(n_modes)
    ph_s = np.zeros(n_modes)

    g_lin = np.array([m.g for m in modes])
    inv_delay = 1.0 / delay_samples
    ln_muted_decay_rate = math.log(MUTED_DECAY_PER_ROUND_TRIP) * inv_delay

    partial_db_hist = np.zeros((n_modes, n_samples))
    ceiling_hits = 0
    muted = False
    saw_nonfinite = False

    target_db = ctrl.target_db
    max_cut_db = ctrl.max_cut_db
    # The master upward unit. Global and post-cell, so it multiplies the round-trip gain
    # of every mode equally -- the one lever here that changes how much energy the loop
    # carries at all, as opposed to which partials the cells hold down.
    master_gain_lin = 10.0 ** (ctrl.master_boost_db / 20.0)
    master_ahead = ctrl.master_ahead_of_cells
    master_db_if_ahead = ctrl.master_boost_db if master_ahead else 0.0
    ceiling = ctrl.output_ceiling

    for n in range(n_samples):
        A = np.exp(ln_A)
        L = math.sqrt(float(np.dot(A, A)))
        if L > 1e-12:
            ratio = L / saturation_limit
            saturation_gain = math.tanh(ratio) / ratio
        else:
            saturation_gain = 1.0

        # ---- composite waveform sample, for the ceiling metric only ----
        # A is the amplitude at the loop point. When the master unit sits AFTER the
        # cells its gain is still to come at the output; when it sits BEFORE them it is
        # already inside what the cells regulated, so it must not be counted twice.
        out = float(np.dot(A, ph_c))
        if not master_ahead:
            out *= master_gain_lin
        if not math.isfinite(out):
            saw_nonfinite = True
            muted = True
        if muted:
            out = 0.0
        elif out > ceiling:
            out = ceiling
            ceiling_hits += 1
        elif out < -ceiling:
            out = -ceiling
            ceiling_hits += 1

        # ---- per-mode detector + control law + growth update ----
        for mi in range(n_modes):
            env = envelopes[mi].process(A[mi])
            partial_db = 20.0 * math.log10(max(env, 1e-9))
            partial_db_hist[mi, n] = partial_db

            if muted:
                rate = ln_muted_decay_rate
            else:
                # With the master unit ahead of the cells, the detector sees the
                # boosted partial -- which is the whole point: the cell then cuts the
                # boost back off anything it has bound, and the lift survives only on
                # partials no cell is holding.
                gain_db = _cell_gain_db(partial_db + master_db_if_ahead, target_db, max_cut_db)
                actuator_gain_lin = 10.0 ** (gain_db / 20.0)
                total_round_trip_gain = (g_lin[mi] * actuator_gain_lin * saturation_gain
                                          * master_gain_lin)
                rate = math.log(max(total_round_trip_gain, 1e-12)) * inv_delay

            ln_A[mi] = _clamp(ln_A[mi] + rate, _LN_FLOOR, _LN_CEIL)
            if not math.isfinite(ln_A[mi]):
                saw_nonfinite = True
                muted = True
                ln_A[mi] = _LN_FLOOR

        # advance the phasors
        new_c = ph_c * cos_w - ph_s * sin_w
        new_s = ph_c * sin_w + ph_s * cos_w
        ph_c, ph_s = new_c, new_s

    # ---------------------------------------------------------------- analysis
    window_n = max(1, int(round(SUSTAIN_WINDOW_S * fs)))
    outcomes: List[ModeOutcome] = []
    settle_times = []
    ripples = []
    for mi, m in enumerate(modes):
        trace = partial_db_hist[mi]
        final_val = float(np.mean(trace[-window_n:]))
        sustained = final_val > SUSTAIN_THRESHOLD_DB
        outcomes.append(ModeOutcome(freq_hz=m.freq_hz, g_open_loop=m.g,
                                     final_partial_db=final_val, sustained=sustained))
        if sustained:
            ripples.append(float(np.std(trace[-window_n:])))
            # settle time: earliest sample after which the trace never again strays more
            # than SETTLE_TOL_DB from its own final value.
            within = np.abs(trace - final_val) <= SETTLE_TOL_DB
            bad = np.nonzero(~within)[0]
            settle_idx = int(bad[-1] + 1) if bad.size else 0
            settle_times.append(settle_idx / fs)

    settle_time_s = max(settle_times) if settle_times else 0.0
    ripple_db = max(ripples) if ripples else 0.0
    diverged = saw_nonfinite or (ceiling_hits / n_samples > DIVERGENCE_CEILING_FRACTION)

    return SimResult(master_boost_db=ctrl.master_boost_db, duration_s=duration_s,
                      modes=outcomes, ceiling_hits=ceiling_hits, n_samples=n_samples,
                      settle_time_s=settle_time_s, ripple_db=ripple_db, diverged=diverged,
                      nonfinite=saw_nonfinite)


# ------------------------------------------------------------ convenience API

def run_baseline(modes: Sequence[Mode] = DEFAULT_PEAKED_MODES, **kwargs) -> SimResult:
    """render.cpp's law with the master upward unit at unity -- the rig as it stands."""
    return simulate(modes, ControllerConfig(), **kwargs)


def run_with_master_boost(master_boost_db: float, modes: Sequence[Mode] = DEFAULT_PEAKED_MODES,
                           **kwargs) -> SimResult:
    """The same law with the master upward unit turned up by master_boost_db.
    master_boost_db = 0 reproduces run_baseline() exactly -- see test_loop_sim.py."""
    return simulate(modes, ControllerConfig(master_boost_db=master_boost_db), **kwargs)


def sweep_master_boost(master_boost_db_values: Sequence[float] = (0, 3, 6, 9, 12, 18),
                        modes: Sequence[Mode] = DEFAULT_PEAKED_MODES, **kwargs) -> List[SimResult]:
    return [run_with_master_boost(b, modes=modes, **kwargs) for b in master_boost_db_values]


def slow_attack_demo(modes: Sequence[Mode] = DEFAULT_PEAKED_MODES, master_boost_db: float = 12.0,
                      fast_attack_ms: float = ATTACK_MS, slow_attack_ms: float = 200.0,
                      **kwargs) -> tuple:
    """Sensitivity check: what happens when the detector's attack is too slow relative to
    the loop's growth rate. Returns (fast_result, slow_result) for the same scenario, the
    attack time the only thing that differs. This is why render.cpp keeps kAttackMs a
    fixed constant and does NOT expose it as one of the live sliders -- ground-rules 6.3
    calls attack a safety margin, not a taste knob."""
    fast = simulate(modes, ControllerConfig(master_boost_db=master_boost_db,
                                             attack_ms=fast_attack_ms), **kwargs)
    slow = simulate(modes, ControllerConfig(master_boost_db=master_boost_db,
                                             attack_ms=slow_attack_ms), **kwargs)
    return fast, slow


if __name__ == "__main__":
    print("=== (1) baseline: master upward unit at unity, peaked g_m distribution ===")
    baseline = run_baseline()
    print(baseline.describe())
    print()

    print("=== (2) sweep over master_boost_db ===")
    for r in sweep_master_boost([0, 3, 6, 9, 12, 18]):
        print(r.describe())
        print()

    print("=== (4) sensitivity: attack too slow relative to loop growth ===")
    fast, slow = slow_attack_demo()
    print("fast attack (%.1f ms):" % ATTACK_MS)
    print(fast.describe())
    print()
    print("slow attack (200.0 ms):")
    print(slow.describe())
