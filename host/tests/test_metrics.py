"""Sanity tests for the proxy metrics, on synthetic signals.

These do not prove the metrics match Abel's ears — nothing here can. They prove the
metrics point the right way on cases where the right answer is not in doubt, so that a
later refactor cannot silently invert them.

    cd host && python3 -m pytest tests/
"""

import numpy as np
import pytest

from harness.metrics import DOMINANCE_CAP_DB, analyse

SR = 44100


def _tone_stack(partials, seconds=3.0, sr=SR):
    t = np.arange(int(seconds * sr)) / sr
    x = sum(a * np.sin(2 * np.pi * f * t) for f, a in partials)
    return x / np.max(np.abs(x))


@pytest.fixture
def winner_takes_all():
    return _tone_stack([(220, 1.0), (443, 0.01), (661, 0.008)])


@pytest.fixture
def rich():
    return _tone_stack([(220, 1.0), (331, 0.8), (443, 0.7), (551, 0.6), (661, 0.5)])


def test_rich_beats_winner_on_every_axis(winner_takes_all, rich):
    w = analyse(winner_takes_all, SR)
    r = analyse(rich, SR)

    assert r.partial_count > w.partial_count
    assert r.dominance_ratio_db < w.dominance_ratio_db
    assert r.peak_flatness > w.peak_flatness


def test_single_partial_is_not_flat(winner_takes_all):
    """One partial is bare, not flat. The formula would say 1.0; we say 0.0."""
    assert analyse(winner_takes_all, SR).peak_flatness == 0.0


def test_dominance_is_finite_when_one_partial_stands_alone(winner_takes_all):
    assert analyse(winner_takes_all, SR).dominance_ratio_db == pytest.approx(
        DOMINANCE_CAP_DB, abs=1.0
    )


def test_silence_is_not_alive():
    m = analyse(np.zeros(SR * 2), SR)
    assert m.sustain_fraction == 0.0
    assert m.alive_at_end is False
    assert m.partial_count == 0.0


def test_take_that_dies_is_not_alive_at_end(rich):
    fade = np.concatenate([np.ones(len(rich) // 2),
                           np.linspace(1, 0, len(rich) - len(rich) // 2) ** 4])
    m = analyse(rich * fade, SR)
    assert m.alive_at_end is False
    assert m.sustain_fraction < 1.0


def test_a_jump_registers_as_a_mode_switch():
    t = np.arange(int(1.5 * SR)) / SR
    x = np.concatenate([np.sin(2 * np.pi * 220 * t), np.sin(2 * np.pi * 370 * t)])
    assert analyse(x, SR).mode_switch_rate_hz > 0.0


def test_a_glide_does_not_register_as_a_mode_switch():
    """Movement inside the glide window is the same partial moving, not a new event."""
    t = np.arange(int(3.0 * SR)) / SR
    f = np.linspace(220.0, 222.0, t.size)          # ~16 cents over the whole take
    x = np.sin(2 * np.pi * np.cumsum(f) / SR)
    assert analyse(x, SR).mode_switch_rate_hz == 0.0
