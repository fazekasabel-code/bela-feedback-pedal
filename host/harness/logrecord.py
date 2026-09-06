"""
Log-record writer for gen1 test runs.

Enforces process rule 10: every result carries its git commit, rig-profile hash,
full parameter set, proxy metrics, and audio path. A result without its parameter
set is not a result.

    from harness.logrecord import write_record, read_records
    path = write_record("take-003", params, metrics, "logs/.../take_003.wav")

    python3 -m harness.logrecord list
"""

from __future__ import annotations

import hashlib
import json
import platform
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

import numpy as np

SCHEMA_VERSION = 1
_GIT_TIMEOUT_S = 2.0
_SLUG_RE = re.compile(r"[^a-z0-9]+")


@dataclass
class Provenance:
    git_commit: str  # short hash, or "unknown" outside a repo
    git_dirty: bool  # True if the working tree has uncommitted changes
    rig_profile_sha256: str  # of rig-profile.json as bytes, or "missing"
    rig_profile_version: int | None
    recorded_at: str  # ISO 8601, local time with offset
    host_platform: str  # platform.platform()


def _resolve_repo_root(repo_root: Path | None) -> Path:
    if repo_root is not None:
        return Path(repo_root).resolve()
    here = Path(__file__).resolve().parent
    for p in [here, *here.parents]:
        if (p / ".git").exists():
            return p
    return Path(__file__).resolve().parents[2]


def _git(args: list[str], cwd: Path) -> str | None:
    try:
        r = subprocess.run(
            ["git", *args],
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=_GIT_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    return r.stdout


def _slugify(run_name: str) -> str:
    slug = _SLUG_RE.sub("-", run_name.lower()).strip("-")
    if not slug:
        raise ValueError(f"run_name {run_name!r} slugifies to an empty filename")
    return slug


def _normalise(value):
    """Convert numpy scalars and arrays to plain Python, recursively.

    The metrics in harness/metrics.py are computed with numpy, so their values arrive
    here as np.float64 / np.bool_, which json refuses. Converting is right and casting
    to str via json's `default=` hook is not: a metric stored as the string "12.5"
    still looks like a record but can never be compared against another run.
    """
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, np.ndarray):
        return [_normalise(v) for v in value.tolist()]
    if isinstance(value, dict):
        return {str(k): _normalise(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_normalise(v) for v in value]
    return value


def _check_jsonable(mapping: dict, label: str) -> dict:
    """Normalise then verify. Raises before anything is written to disk."""
    out = {}
    for key, value in mapping.items():
        clean = _normalise(value)
        try:
            json.dumps(clean)
        except (TypeError, ValueError) as exc:
            raise TypeError(
                f"{label}[{key!r}] is not JSON-serialisable: {exc}"
            ) from exc
        out[key] = clean
    return out


def _audio_path_for_record(audio_path: Path | str, repo_root: Path) -> str:
    path = Path(audio_path)
    if not path.is_absolute():
        path = (repo_root / path).resolve()
    else:
        path = path.resolve()
    try:
        return str(path.relative_to(repo_root))
    except ValueError:
        return str(path)


def collect_provenance(repo_root: Path | None = None) -> Provenance:
    root = _resolve_repo_root(repo_root)

    git_commit = "unknown"
    git_dirty = False
    out = _git(["rev-parse", "--short", "HEAD"], root)
    if out is not None:
        git_commit = out.strip() or "unknown"
        status = _git(["status", "--porcelain"], root)
        if status is not None:
            git_dirty = bool(status.strip())

    sha = "missing"
    version: int | None = None
    profile = root / "rig-profile.json"
    if profile.is_file():
        raw = profile.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        try:
            version = json.loads(raw.decode("utf-8")).get("version")
            if version is not None:
                version = int(version)
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
            version = None

    return Provenance(
        git_commit=git_commit,
        git_dirty=git_dirty,
        rig_profile_sha256=sha,
        rig_profile_version=version,
        recorded_at=datetime.now().astimezone().isoformat(),
        host_platform=platform.platform(),
    )


def write_record(
    run_name: str,
    params: dict,
    metrics: dict | None,
    audio_path: Path | str,
    notes: str = "",
    repo_root: Path | None = None,
) -> Path:
    slug = _slugify(run_name)
    # Validate and normalise BEFORE touching the filesystem, so a bad record never
    # leaves a half-made directory behind.
    params = _check_jsonable(params, "params")
    if metrics is not None:
        metrics = _check_jsonable(metrics, "metrics")

    root = _resolve_repo_root(repo_root)
    now = datetime.now().astimezone()
    day_dir = root / "logs" / now.strftime("%Y-%m-%d")
    day_dir.mkdir(parents=True, exist_ok=True)

    out_path = day_dir / f"{now.strftime('%H%M%S')}_{slug}.json"
    record = {
        "schema_version": SCHEMA_VERSION,
        "run_name": slug,
        "provenance": asdict(collect_provenance(root)),
        "params": params,
        "metrics": metrics,
        "audio_path": _audio_path_for_record(audio_path, root),
        "notes": notes,
    }
    out_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return out_path


def read_records(repo_root: Path | None = None) -> list[dict]:
    root = _resolve_repo_root(repo_root)
    logs = root / "logs"
    if not logs.is_dir():
        return []

    records: list[tuple[str, dict]] = []
    for path in logs.glob("*/*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        key = data.get("provenance", {}).get("recorded_at") or path.as_posix()
        records.append((key, data))

    records.sort(key=lambda item: item[0], reverse=True)
    return [data for _, data in records]


def _cli_list(repo_root: Path | None = None) -> None:
    for rec in read_records(repo_root):
        prov = rec.get("provenance") or {}
        date = (prov.get("recorded_at") or "")[:10] or "?"
        commit = prov.get("git_commit") or "unknown"
        if prov.get("git_dirty"):
            commit += "*"
        has_metrics = "metrics" if rec.get("metrics") is not None else "no-metrics"
        print(f"{date}  {rec.get('run_name', '?')}  {commit}  {has_metrics}")


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv == ["list"]:
        _cli_list()
        return 0
    print("usage: python3 -m harness.logrecord list", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
