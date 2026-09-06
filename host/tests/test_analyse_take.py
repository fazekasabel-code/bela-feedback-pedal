"""Sanity tests for `rig.analyse_take`, on synthetic WAVs built with the stdlib.

Offline only, on numpy arrays written to tmp_path with the stdlib `wave` module —
never on a real recording, never opening an audio stream. See CLAUDE.md: another
process may be holding the interface, and this tool has no business touching it.

    cd host && python3 -m pytest tests/test_analyse_take.py -q
"""

import wave

import numpy as np
import pytest

from rig.analyse_take import DOMINANCE_CAP_DB, analyse_take, hz_to_note

SR = 44100


def _write_wav(path, x: np.ndarray, sr: int = SR) -> str:
    """Mono float in -1..1 -> 16-bit PCM WAV, same convention as rig.loop._write_wav."""
    clipped = np.clip(x, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())
    return str(path)


def _sine(freq, seconds, amp=0.9, sr=SR):
    t = np.arange(int(seconds * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float64)


def _tone_stack(partials, seconds=3.0, sr=SR):
    t = np.arange(int(seconds * sr)) / sr
    x = sum(a * np.sin(2 * np.pi * f * t) for f, a in partials)
    return x / np.max(np.abs(x))


# ------------------------------------------------------------------- fixtures

@pytest.fixture
def pure_tone_wav(tmp_path):
    return _write_wav(tmp_path / "pure.wav", _sine(220.0, 3.0))


@pytest.fixture
def three_partial_wav(tmp_path):
    x = _tone_stack([(220, 1.0), (440, 0.9), (660, 0.8)])
    return _write_wav(tmp_path / "rich.wav", x)


@pytest.fixture
def silence_wav(tmp_path):
    return _write_wav(tmp_path / "silence.wav", np.zeros(int(2.0 * SR)))


@pytest.fixture
def delayed_onset_wav(tmp_path):
    x = np.concatenate([np.zeros(int(1.0 * SR)), _sine(220.0, 2.0)])
    return _write_wav(tmp_path / "delayed.wav", x)


# ------------------------------------------------------------------- tests

def test_pure_tone_dominant_frequency_and_sustain(pure_tone_wav):
    a = analyse_take(pure_tone_wav)
    assert a.dominant_hz == pytest.approx(220.0, abs=1.0)
    assert a.sustained_fraction > 0.9


def test_three_partial_stack_has_at_least_three_partials(three_partial_wav):
    a = analyse_take(three_partial_wav)
    assert a.mean_partials >= 3.0


def test_pure_tone_dominance_is_worse_than_three_partial_stack(
    pure_tone_wav, three_partial_wav
):
    pure = analyse_take(pure_tone_wav)
    rich = analyse_take(three_partial_wav)
    # dominance_db: LOWER is better, so the single sine must score higher (worse).
    assert pure.dominance_db > rich.dominance_db


def test_silence_does_not_crash_and_has_zero_sustain(silence_wav):
    a = analyse_take(silence_wav)
    assert a.sustained_fraction == 0.0
    assert a.mean_partials == 0.0
    assert a.p10_partials == 0.0
    assert a.dominance_db == pytest.approx(DOMINANCE_CAP_DB)
    assert a.feedback_onset_s is None
    assert a.top_partials == []


def test_onset_after_one_second_of_silence(delayed_onset_wav):
    a = analyse_take(delayed_onset_wav)
    assert a.feedback_onset_s is not None
    assert a.feedback_onset_s == pytest.approx(1.0, abs=0.15)


def test_silence_has_no_onset(silence_wav):
    assert analyse_take(silence_wav).feedback_onset_s is None


def test_note_naming_a4():
    name, cents = hz_to_note(440.0)
    assert name == "A4"
    assert cents == pytest.approx(0.0, abs=1.0)


def test_note_naming_b3():
    name, cents = hz_to_note(246.94)
    assert name == "B3"
    assert cents == pytest.approx(0.0, abs=1.0)


def test_dominant_stability_is_tight_for_a_pure_steady_tone(pure_tone_wav):
    a = analyse_take(pure_tone_wav)
    # a steady sine should not wander by much once quantisation is interpolated away
    assert a.dominant_stability_cents < 5.0


def test_load_wav_roundtrip_shape_and_range(pure_tone_wav):
    from rig.analyse_take import load_wav

    data, sr = load_wav(pure_tone_wav)
    assert sr == SR
    assert data.ndim == 2
    assert data.shape[1] == 1
    assert np.max(np.abs(data)) <= 1.0 + 1e-6


def test_channel_selection_reads_the_right_channel(tmp_path):
    sr = SR
    n = int(2.0 * sr)
    ch0 = _sine(220.0, 2.0, sr=sr)          # ch0 = input: 220 Hz
    ch1 = _sine(660.0, 2.0, sr=sr)          # ch1 = output: 660 Hz
    stereo = np.stack([ch0, ch1], axis=1)
    clipped = np.clip(stereo, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")
    path = tmp_path / "stereo.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())

    a0 = analyse_take(str(path), channel=0)
    a1 = analyse_take(str(path), channel=1)
    assert a0.dominant_hz == pytest.approx(220.0, abs=1.0)
    assert a1.dominant_hz == pytest.approx(660.0, abs=1.0)


def test_to_dict_is_json_shaped(pure_tone_wav):
    a = analyse_take(pure_tone_wav)
    d = a.to_dict()
    assert d["path"] == pure_tone_wav
    assert isinstance(d["top_partials"], list)
