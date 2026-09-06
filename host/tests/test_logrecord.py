"""Tests for the log-record writer.

    cd host && python3 -m pytest tests/test_logrecord.py -q
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from harness.logrecord import (
    collect_provenance,
    read_records,
    write_record,
)
from harness import logrecord as logrecord_mod


def test_round_trip_write_then_read(tmp_path: Path):
    audio = tmp_path / "takes" / "a.wav"
    audio.parent.mkdir()
    audio.write_bytes(b"RIFF")

    path = write_record(
        "take 003",
        {"gain": 0.5, "cells": 4},
        {"partial_count": 2.1},
        audio,
        notes="first try",
        repo_root=tmp_path,
    )
    assert path.is_file()
    assert path.parent.parent == tmp_path / "logs"

    records = read_records(tmp_path)
    assert len(records) == 1
    rec = records[0]
    assert rec["schema_version"] == 1
    assert rec["run_name"] == "take-003"
    assert rec["params"] == {"gain": 0.5, "cells": 4}
    assert rec["metrics"] == {"partial_count": 2.1}
    assert rec["notes"] == "first try"
    assert "provenance" in rec
    assert rec["audio_path"] == "takes/a.wav"


def test_slugifier_rejects_empty(tmp_path: Path):
    with pytest.raises(ValueError, match="empty"):
        write_record("!!!", {}, None, tmp_path / "x.wav", repo_root=tmp_path)
    assert list((tmp_path / "logs").glob("*/*")) == [] if (tmp_path / "logs").exists() else True


def test_slugifier_normalises(tmp_path: Path):
    path = write_record(
        "Take_003 -- Final!",
        {"ok": True},
        None,
        tmp_path / "a.wav",
        repo_root=tmp_path,
    )
    assert path.name.endswith("_take-003-final.json")
    assert json.loads(path.read_text())["run_name"] == "take-003-final"


def test_non_serialisable_param_raises_and_writes_nothing(tmp_path: Path):
    with pytest.raises(TypeError, match=r"params\['bad'\]"):
        write_record(
            "broken",
            {"ok": 1, "bad": object()},
            None,
            tmp_path / "a.wav",
            repo_root=tmp_path,
        )
    logs = tmp_path / "logs"
    assert not logs.exists() or list(logs.rglob("*.json")) == []


def test_metrics_none_accepted(tmp_path: Path):
    path = write_record(
        "unscored",
        {"x": 1},
        None,
        tmp_path / "a.wav",
        repo_root=tmp_path,
    )
    rec = json.loads(path.read_text())
    assert rec["metrics"] is None
    assert read_records(tmp_path)[0]["metrics"] is None


def test_audio_path_relative_inside_absolute_outside(tmp_path: Path, tmp_path_factory):
    inside = tmp_path / "audio" / "in.wav"
    inside.parent.mkdir()
    inside.write_bytes(b"x")

    outside_root = tmp_path_factory.mktemp("outside")
    outside = outside_root / "out.wav"
    outside.write_bytes(b"y")

    p_in = write_record("in", {}, None, inside, repo_root=tmp_path)
    p_out = write_record("out", {}, None, outside, repo_root=tmp_path)

    assert json.loads(p_in.read_text())["audio_path"] == "audio/in.wav"
    stored_out = json.loads(p_out.read_text())["audio_path"]
    assert Path(stored_out).is_absolute()
    assert Path(stored_out) == outside.resolve()


def test_provenance_degrades_outside_git_repo(tmp_path: Path):
    (tmp_path / "rig-profile.json").write_text(
        json.dumps({"version": 3}), encoding="utf-8"
    )
    prov = collect_provenance(tmp_path)
    assert prov.git_commit == "unknown"
    assert prov.git_dirty is False
    assert prov.rig_profile_version == 3
    assert len(prov.rig_profile_sha256) == 64
    assert prov.rig_profile_sha256 != "missing"
    assert "T" in prov.recorded_at
    assert prov.host_platform

    bare = collect_provenance(tmp_path / "no-profile-here")
    # non-existent root: still degrades, profile missing
    assert bare.git_commit == "unknown"
    assert bare.rig_profile_sha256 == "missing"
    assert bare.rig_profile_version is None


def test_read_records_newest_first(tmp_path: Path, monkeypatch):
    from datetime import datetime, timezone

    current = {"t": datetime(2026, 9, 6, 10, 0, 0, tzinfo=timezone.utc)}

    class _FakeDateTime:
        @classmethod
        def now(cls, tz=None):
            return current["t"]

    monkeypatch.setattr(logrecord_mod, "datetime", _FakeDateTime)
    write_record("older", {"n": 1}, None, tmp_path / "a.wav", repo_root=tmp_path)
    current["t"] = datetime(2026, 9, 6, 11, 0, 0, tzinfo=timezone.utc)
    write_record("newer", {"n": 2}, None, tmp_path / "b.wav", repo_root=tmp_path)

    names = [r["run_name"] for r in read_records(tmp_path)]
    assert names == ["newer", "older"]


# --- added during review: the metrics path is where numpy actually shows up --------

def test_numpy_metrics_are_stored_as_numbers_not_strings(tmp_path):
    """harness.metrics computes with numpy, so its values arrive as np scalars.

    They must land in the record as JSON numbers. Stored as strings they would still
    look like a result but could never be compared against another run.
    """
    import json as _json
    import numpy as _np

    from harness.logrecord import write_record

    path = write_record(
        "numpy-metrics",
        {"gain_db": _np.float64(-20.0)},
        {"dominance_ratio_db": _np.float64(12.5), "alive_at_end": _np.bool_(True),
         "partials": _np.array([220.0, 440.0])},
        "take.wav",
        repo_root=tmp_path,
    )
    rec = _json.loads(path.read_text())
    assert rec["metrics"]["dominance_ratio_db"] == 12.5
    assert isinstance(rec["metrics"]["dominance_ratio_db"], float)
    assert rec["metrics"]["alive_at_end"] is True
    assert rec["metrics"]["partials"] == [220.0, 440.0]
    assert isinstance(rec["params"]["gain_db"], float)


def test_bad_metrics_raise_before_creating_anything(tmp_path):
    from harness.logrecord import write_record

    with pytest.raises(TypeError, match="metrics"):
        write_record("bad", {}, {"x": object()}, "take.wav", repo_root=tmp_path)
    assert not (tmp_path / "logs").exists()
