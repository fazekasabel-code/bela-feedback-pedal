"""
Time-windowed partial snapshots for a recorded take: chunk the recording into fixed
windows and run `rig.analyse_take`'s existing scoring on each chunk, so "is this
getting better or worse over the length of the take" can be read directly instead of
inferred from one whole-take average.

Built 2026-09-13 to compare bela/gen1-cell's PASSED take against bela/gen1-multicell's
two takes that read closer to single-partial -- a single aggregate number (mean
partials over 15-40s) can't distinguish "never built up" from "built up then died" from
"was multi-partial the whole time but briefly dipped", and Abel asked for exactly this
kind of comparison rather than another aggregate metric.

Reuses `rig.analyse_take.analyse_take` unchanged on each chunk (written to a temp WAV)
rather than re-deriving the STFT/peak-picking logic -- that function is already the
project's one offline scorer, this just calls it repeatedly on slices.

    cd host && python3 -m rig.snapshot_partials ../logs/2026-09-13/foo_outputs.wav
    cd host && python3 -m rig.snapshot_partials ../logs/2026-09-13/foo_outputs.wav \
        --window 5 --channel 0

Offline only. Nothing here opens an audio stream.
"""
from __future__ import annotations

import argparse
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

from rig.analyse_take import analyse_take, hz_to_note, load_wav

DEFAULT_WINDOW_S = 5.0


def _write_chunk_wav(path: Path, chunk: np.ndarray, sr: int) -> None:
    pcm16 = np.clip(chunk * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm16.tobytes())


def snapshot(path, channel: int = 0, window_s: float = DEFAULT_WINDOW_S) -> list[dict]:
    """Return one analyse_take() result (as a dict, plus t_start_s/t_end_s) per window."""
    data, sr = load_wav(path)
    if channel >= data.shape[1]:
        raise ValueError(f"channel {channel} not in {data.shape[1]}-channel file {path}")
    x = data[:, channel].astype(np.float64)

    window_n = max(1, int(window_s * sr))
    n_chunks = max(1, int(np.ceil(x.size / window_n)))

    results = []
    with tempfile.TemporaryDirectory(prefix="snapshot_partials_") as tmp:
        for i in range(n_chunks):
            start = i * window_n
            end = min(start + window_n, x.size)
            chunk = x[start:end]
            if chunk.size < 4096:  # analyse_take's own WINDOW -- too short to score
                continue
            chunk_path = Path(tmp) / f"chunk_{i}.wav"
            _write_chunk_wav(chunk_path, chunk, sr)
            a = analyse_take(chunk_path, channel=0)
            d = a.to_dict()
            d["t_start_s"] = start / sr
            d["t_end_s"] = end / sr
            results.append(d)
    return results


def _print_human(path: str, rows: list[dict]) -> None:
    print(path)
    for d in rows:
        top = "  ".join(
            f"{p['freq_hz']:.0f}Hz({hz_to_note(p['freq_hz'])[0]},{p['level_db']:+.1f}dB)"
            for p in d["top_partials"]
        ) or "(none above the silence floor)"
        print(
            f"  [{d['t_start_s']:5.1f}-{d['t_end_s']:5.1f}s] "
            f"partials(mean/p10)={d['mean_partials']:.2f}/{d['p10_partials']:.2f}  "
            f"dominance={d['dominance_db']:6.1f}dB  sustained={d['sustained_fraction']:.2f}  "
            f"top: {top}"
        )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="rig.snapshot_partials", description=__doc__.split("\n\n")[0],
    )
    p.add_argument("path", help="WAV to analyse (ch0=rig input, ch1=DSP output)")
    p.add_argument("--channel", type=int, default=0, help="channel to analyse (default 0)")
    p.add_argument("--window", type=float, default=DEFAULT_WINDOW_S,
                   help=f"snapshot window in seconds (default {DEFAULT_WINDOW_S:.0f})")
    p.add_argument("--json", action="store_true", help="emit one JSON list")
    args = p.parse_args(argv)

    try:
        rows = snapshot(args.path, channel=args.channel, window_s=args.window)
    except (OSError, wave.Error, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.json:
        import json
        print(json.dumps(rows, indent=2, default=str))
    else:
        _print_human(args.path, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
