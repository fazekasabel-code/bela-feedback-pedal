"""
Offline analysis for bela/latency-check/ -- the real mechanical round-trip latency
measurement (ground-rules-and-facts.md 3.1/11's open question).

latency-check emits kNumBursts short, quiet tone bursts on the exciter's output
channel at known times (kLeadInS + i*kBurstSpacingS) and records both channels.
This finds each burst's actual arrival time on the pickup channel by normalised
cross-correlation -- same method as host/rig/io_check.py's cmd_latency, adapted
from a direct electrical loopback to the real exciter->body->strings->pickup
path, so the number it reports is the WHOLE round trip, mechanical leg included.

    cd host && .venv/bin/python -m rig.analyse_latency \
        ../logs/2026-09-13/latency_check_inputs.wav

The known-good burst parameters below must match bela/latency-check/render.cpp
exactly, or the correlation template won't match what's actually in the
recording.
"""
from __future__ import annotations

import argparse
import math
import sys
import wave

import numpy as np

# Must match bela/latency-check/render.cpp's constants exactly.
SR = 44100
BURST_HZ = 1000.0
BURST_DURATION_S = 0.002
BURST_SPACING_S = 1.0
NUM_BURSTS = 5
LEAD_IN_S = 0.3
BURST_AMP = 0.08

NCC_MIN = 0.5
SEARCH_WINDOW_S = 0.05  # how far past the nominal emit time to search for arrival.
# Narrowed from an initial 0.3s, 2026-09-13: at 0.3s two of five bursts matched the
# guitar body's own resonant ringing after the real burst (similar score, wildly
# different delay) rather than the burst's actual arrival. The three consistent
# readings were all under 5ms, so 50ms is ample margin without reopening that door.


def _make_burst() -> np.ndarray:
    n = max(1, int(BURST_DURATION_S * SR))
    t = np.arange(n, dtype=np.float64) / SR
    window = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(n) / (n - 1)))
    return (BURST_AMP * np.sin(2.0 * np.pi * BURST_HZ * t) * window).astype(np.float64)


def _ncc_delay(recording: np.ndarray, burst: np.ndarray) -> tuple[int, float]:
    corr = np.correlate(recording, burst, mode="valid")
    burst_e = float(np.dot(burst, burst))
    c = np.concatenate([[0.0], np.cumsum(recording * recording)])
    L = len(burst)
    win_e = c[L:] - c[:-L]
    ncc = corr / (np.sqrt(win_e * burst_e) + 1e-12)
    i = int(np.argmax(ncc))
    return i, float(ncc[i])


def load_wav(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        sr = w.getframerate()
        n_frames = w.getnframes()
        raw = w.readframes(n_frames)
    if sampwidth == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"unsupported sample width: {sampwidth} bytes")
    return data.reshape(-1, n_channels), sr


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("inputs_wav", help="latency-check's inputs.wav (pickup channel)")
    p.add_argument("--channel", type=int, default=0, help="pickup channel (default 0)")
    args = p.parse_args(argv)

    data, sr = load_wav(args.inputs_wav)
    if sr != SR:
        print(f"warning: recording sample rate {sr} != expected {SR}", file=sys.stderr)
    x = data[:, args.channel]
    burst = _make_burst()

    delays_ms, scores = [], []
    for i in range(NUM_BURSTS):
        emit_s = LEAD_IN_S + i * BURST_SPACING_S
        emit_sample = int(emit_s * sr)
        search_start = emit_sample
        search_end = emit_sample + int(SEARCH_WINDOW_S * sr) + len(burst)
        window = x[search_start:search_end]
        if len(window) < len(burst):
            print(f"burst {i+1}/{NUM_BURSTS}: recording too short to search, skipping")
            continue
        found, score = _ncc_delay(window, burst)
        delay_samples = found
        delay_ms = delay_samples / sr * 1000.0
        if score < NCC_MIN:
            print(f"burst {i+1}/{NUM_BURSTS}: NO MATCH (score={score:.2f}) -- "
                  f"burst not found in the pickup recording within {SEARCH_WINDOW_S}s")
            continue
        delays_ms.append(delay_ms)
        scores.append(score)
        print(f"burst {i+1}/{NUM_BURSTS}: round-trip delay = {delay_ms:.2f} ms "
              f"({delay_samples} samples)  score={score:.2f}")

    if len(delays_ms) < 3:
        print(f"\nerror: only {len(delays_ms)}/{NUM_BURSTS} bursts found -- "
              "not a trustworthy measurement. Check the exciter is actually "
              "mounted and coupled to the body, and that the pickup channel is right.",
              file=sys.stderr)
        return 1

    med_ms = float(np.median(delays_ms))
    spread_ms = float(max(delays_ms) - min(delays_ms))
    print(f"\nmedian round-trip latency: {med_ms:.2f} ms  "
          f"(spread {spread_ms:.2f} ms across {len(delays_ms)} bursts, "
          f"score_min={min(scores):.2f})")
    print("This is the WHOLE loop: Bela DAC -> DTA120 -> exciter -> body -> "
          "strings -> pickup -> Bela ADC -- electrical and mechanical/acoustic "
          "legs together.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
