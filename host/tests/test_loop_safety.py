"""
Tests for the one piece of arithmetic standing between a runaway loop and the exciter.

These are not "does the DSP sound right" tests — there is nothing musical here yet.
They check that the ceiling actually bounds the output, that the gain ramp is a ramp
and not a step, and that no path through the block processor can hand the exciter a
sample larger than CEILING. See CLAUDE.md safety rules 1 and 9.
"""

import numpy as np
import pytest

from rig.loop import CEILING, MAX_GAIN_DB, MIN_GAIN_DB, _db_to_lin, apply_gain_and_ceiling


def _scratch(n):
    return np.zeros(n, dtype=np.float32), np.zeros(n, dtype=np.float32)


def test_ceiling_bounds_a_signal_far_above_it():
    src = np.full(128, 10.0, dtype=np.float32)   # absurdly hot, as a runaway would be
    dst, ramp = _scratch(128)
    hits = apply_gain_and_ceiling(src, 1.0, 1.0, dst, ramp)
    assert hits == 128
    assert np.max(np.abs(dst)) <= CEILING


def test_ceiling_bounds_the_output_at_every_reachable_gain():
    # Full-scale input at the maximum software gain must still leave the ceiling intact.
    src = np.concatenate([np.full(64, 1.0), np.full(64, -1.0)]).astype(np.float32)
    dst, ramp = _scratch(128)
    for db in np.arange(MIN_GAIN_DB, MAX_GAIN_DB + 0.5, 0.5):
        g = _db_to_lin(float(db))
        apply_gain_and_ceiling(src, g, g, dst, ramp)
        assert np.max(np.abs(dst)) <= CEILING, f"ceiling breached at {db} dB"


def test_ramping_gain_also_respects_the_ceiling():
    src = np.full(128, 1.0, dtype=np.float32)
    dst, ramp = _scratch(128)
    apply_gain_and_ceiling(src, _db_to_lin(-60.0), _db_to_lin(MAX_GAIN_DB), dst, ramp)
    assert np.max(np.abs(dst)) <= CEILING


def test_quiet_signal_is_untouched_and_counts_no_hits():
    src = (np.random.RandomState(0).randn(128) * 0.01).astype(np.float32)
    dst, ramp = _scratch(128)
    hits = apply_gain_and_ceiling(src, 1.0, 1.0, dst, ramp)
    assert hits == 0
    assert np.allclose(dst[:128], src, atol=1e-6)


def test_gain_change_is_a_ramp_not_a_step():
    src = np.ones(128, dtype=np.float32)
    dst, ramp = _scratch(128)
    apply_gain_and_ceiling(src, 0.0, 0.5, dst, ramp)
    # Monotone from the old gain to the new one, and no jump bigger than one step.
    assert dst[0] == pytest.approx(0.0, abs=1e-6)
    assert np.all(np.diff(dst[:128]) >= -1e-7)
    assert np.max(np.abs(np.diff(dst[:128]))) < 0.5 / 64


def test_mute_is_exactly_zero_not_merely_quiet():
    # -80 dB and below is silence: a "very small gain" would still let a runaway
    # loop climb back. _db_to_lin must return a hard zero.
    assert _db_to_lin(MIN_GAIN_DB) == 0.0
    assert _db_to_lin(MIN_GAIN_DB - 20.0) == 0.0
    src = np.full(128, 1.0, dtype=np.float32)
    dst, ramp = _scratch(128)
    apply_gain_and_ceiling(src, 0.0, 0.0, dst, ramp)
    assert np.count_nonzero(dst) == 0


def test_processor_allocates_nothing_it_was_not_given():
    # dst and ramp are caller-owned; the function must write through them, because in
    # the real callback they are the preallocated buffers and an allocation is a dropout.
    src = np.full(64, 0.5, dtype=np.float32)
    dst, ramp = _scratch(64)
    dst_id, ramp_id = id(dst), id(ramp)
    apply_gain_and_ceiling(src, 0.5, 1.0, dst, ramp)
    assert id(dst) == dst_id and id(ramp) == ramp_id
    assert np.any(dst != 0.0)
