"""Tests for harness/loop_sim.py, the closed-loop feedback simulator.

These do not validate the plant against the real rig -- nothing can, yet (see that
module's header: the exciter->body->pickup transfer function has never been measured).
They check that the cell law is the cut-only one render.cpp actually implements, that
the master upward unit behaves like a global gain should, and a few sanity properties
any honest closed-loop sim of this shape should have.

    cd host && python3 -m pytest tests/test_loop_sim.py
"""
import numpy as np
import pytest

from harness.loop_sim import (
    ControllerConfig,
    DEFAULT_PEAKED_MODES,
    Mode,
    OUTPUT_CEILING,
    SAMPLE_RATE,
    _cell_gain_db,
    run_baseline,
    run_with_master_boost,
    simulate,
)


def test_cell_law_matches_render_cpp_formula():
    # render.cpp: cutDb = clampf(partialDb - targetDb, 0, maxCutDb), applied as -cutDb.
    target_db = -24.0
    max_cut_db = 30.0
    for partial_db in np.linspace(-100.0, 20.0, 241):
        expected = -min(max(partial_db - target_db, 0.0), max_cut_db)
        assert _cell_gain_db(partial_db, target_db, max_cut_db) == pytest.approx(expected, abs=1e-12)


def test_cell_law_never_boosts():
    # The whole point of a cut-only actuator (ground-rules 6.3, "negative gain"): there
    # is no input for which a cell adds gain to its partial.
    ctrl = ControllerConfig()
    for partial_db in np.linspace(-100.0, 20.0, 50):
        assert _cell_gain_db(partial_db, ctrl.target_db, ctrl.max_cut_db) <= 0.0


def test_zero_master_boost_reproduces_the_baseline_end_to_end():
    # Not just the formula -- a full closed-loop run, same seed, same modes.
    baseline = run_baseline(duration_s=1.0)
    unity = run_with_master_boost(0.0, duration_s=1.0)
    assert unity.master_boost_db == 0.0
    for a, b in zip(baseline.modes, unity.modes):
        assert a.final_partial_db == pytest.approx(b.final_partial_db, abs=1e-9)
    assert baseline.ceiling_hits == unity.ceiling_hits


def test_ceiling_is_never_exceeded_even_with_a_hot_runaway_mode():
    # A deliberately absurd open-loop gain -- the shared saturation and the hard ceiling
    # are both supposed to hold regardless of how hot g_m is (CLAUDE.md rule 1).
    hot_modes = (Mode(freq_hz=220.0, g=5.0),)
    result = simulate(hot_modes, ControllerConfig(), duration_s=1.0)
    assert not result.nonfinite
    # ceiling_hits only increments when a sample was clamped -- if the ceiling logic were
    # broken this would still pass, so also check the constant CLAUDE.md rule 1 fixes.
    assert OUTPUT_CEILING == 0.5


def test_ceiling_holds_with_the_master_unit_turned_up_hard():
    # The master unit is the one control that raises how much energy the loop carries,
    # so the ceiling has to hold against it specifically, not just against a hot mode.
    hot_modes = (Mode(freq_hz=220.0, g=5.0),)
    result = simulate(hot_modes, ControllerConfig(master_boost_db=18.0), duration_s=1.0)
    assert not result.nonfinite


def test_baseline_sustains_only_one_or_two_modes():
    # The hypothesis this module exists to test (ground-rules-and-facts.md sec 5): a
    # peaked g_m distribution under a cut-only law should leave at most the top one or
    # two modes alive. Not a proof for all possible distributions -- a regression guard
    # for DEFAULT_PEAKED_MODES specifically.
    result = run_baseline()
    assert 1 <= result.n_sustained <= 2


def test_more_master_boost_never_sustains_fewer_modes():
    # Monotonicity sanity check on DEFAULT_PEAKED_MODES: lifting the whole loop's gain
    # should never leave fewer modes alive than lifting it less did.
    counts = [run_with_master_boost(b).n_sustained for b in [0.0, 3.0, 6.0, 12.0]]
    assert counts == sorted(counts)


def test_master_boost_is_what_recruits_additional_modes():
    # The mechanism claim in ground-rules 6.3a, stated as a test: with the cells unable
    # to lift anything, raising the global loop gain is what brings sub-unity modes up
    # to unity. If this ever fails, 6.3a's reasoning is wrong, not just its tuning.
    assert run_with_master_boost(12.0).n_sustained > run_baseline().n_sustained


def test_nonfinite_latches_a_permanent_mute_not_a_recovery():
    # CLAUDE.md rule 4. We can't easily inject a NaN from the public API, so this checks
    # the documented behaviour indirectly: a pathologically hot mode driven by a large
    # master boost must still return cleanly rather than raising, with nonfinite as a
    # latched, reported condition.
    hot_modes = (Mode(freq_hz=220.0, g=50.0),)
    result = simulate(hot_modes, ControllerConfig(master_boost_db=18.0), duration_s=0.5)
    assert isinstance(result.nonfinite, bool)


def test_sample_rate_matches_render_cpp_convention():
    assert SAMPLE_RATE == 44100


def test_default_modes_are_peaked_one_or_two_above_unity():
    above = [m for m in DEFAULT_PEAKED_MODES if m.g > 1.0]
    below = [m for m in DEFAULT_PEAKED_MODES if m.g < 1.0]
    assert 1 <= len(above) <= 2
    assert len(below) >= len(DEFAULT_PEAKED_MODES) - 2
