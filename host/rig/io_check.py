"""
Phase M0 I/O bring-up for the Mac + Focusrite Scarlett 8i6 rig.

Opens the Scarlett at 48 kHz / 128 frames, confirms channel map by observation,
checks input gain, counts xruns, measures electrical round-trip latency.

Does not close the feedback loop or drive the exciter. Power amp OFF for every
subcommand that opens an output. Cannot verify Focusrite Control Playback 3/4 —
check that by eye.  cd host/ && python3 -m rig.io_check <subcommand>
"""

from __future__ import annotations

import argparse
import gc
import json
import math
import sys
import time
from dataclasses import asdict, dataclass

import numpy as np
import sounddevice as sd

SR = 48000
BLOCK = 128
DEVICE_HINT = "Scarlett"
TEST_TONE_AMP = 0.05  # never raise this
TEST_TONE_HZ = 440.0

# sounddevice defaults to latency="high", which on this interface asked for ~90 ms of
# buffering in each direction. In a feedback loop that is not a comfort setting: loop
# delay sets which partials satisfy the 360*n phase condition, so 180 ms round trip
# would pack the comb teeth about 5 Hz apart and make the jump budget of docs section 7
# unreachable. Always ask for "low"; the electrical `latency` measurement is the number
# that actually counts.
LATENCY = "low"
_EPS = 1e-12

@dataclass
class DevicesResult:
    found: bool
    scarlett_indices: list
    devices: list

@dataclass
class XrunsResult:
    blocks: int
    input_overflows: int
    output_underflows: int
    max_callback_ms: float
    mean_callback_ms: float
    latency_in_ms: float
    latency_out_ms: float
    ok: bool

@dataclass
class MapResult:
    out_channel: int
    seconds: float
    input_rms: list

@dataclass
class MeterResult:
    channel: int
    seconds: float
    peak_dbfs: float
    verdict: str  # "ok" | "too_hot" | "too_quiet"

@dataclass
class LatencyResult:
    out_channel: int
    in_channel: int
    delays_samples: list
    delays_ms: list
    median_samples: float
    median_ms: float
    spread_ms: float
    spread_ok: bool
    trials_found: int      # out of 5. Fewer than 3 and this is not a measurement.
    score_min: float       # weakest NCC match kept. Near 1.0 is a clean detection.


def _dbfs(x: float) -> float:
    return 20.0 * math.log10(max(abs(x), _EPS))

def _emit(result, as_json: bool, text: str) -> None:
    print(json.dumps(asdict(result), indent=2, default=str) if as_json else text)

def _scarlett_hits() -> list[tuple[int, dict]]:
    return [
        (i, d) for i, d in enumerate(sd.query_devices())
        if DEVICE_HINT.lower() in d["name"].lower()
    ]

def _resolve_device(need_in: int = 0, need_out: int = 0):
    hits = _scarlett_hits()
    if not hits:
        print(f"error: no device whose name contains {DEVICE_HINT!r}", file=sys.stderr)
        sys.exit(1)
    for i, d in hits:
        if d["max_input_channels"] >= need_in and d["max_output_channels"] >= need_out:
            return i
    in_dev = next((i for i, d in hits if d["max_input_channels"] >= need_in), None)
    out_dev = next((i for i, d in hits if d["max_output_channels"] >= need_out), None)
    if need_in and need_out and in_dev is not None and out_dev is not None:
        return (in_dev, out_dev)
    if need_in and not need_out and in_dev is not None:
        return in_dev
    if need_out and not need_in and out_dev is not None:
        return out_dev
    print(f"error: Scarlett lacks {need_in} in / {need_out} out", file=sys.stderr)
    sys.exit(1)

def _require_amp_off(confirmed: bool) -> None:
    if not confirmed:
        print(
            "error: refuse to open outputs without --i-confirm-amp-is-off "
            "(power amp must be OFF)",
            file=sys.stderr,
        )
        sys.exit(1)
    print("*** POWER AMP MUST BE OFF — outputs will go live ***", flush=True)

def _make_burst() -> np.ndarray:
    n = max(1, int(0.002 * SR))
    t = np.arange(n, dtype=np.float64) / SR
    return (TEST_TONE_AMP * np.sin(2.0 * math.pi * 1000.0 * t) * np.hanning(n)).astype(
        np.float32
    )

NCC_MIN = 0.5          # below this we did not find the burst, we found noise
NCC_ENERGY_FLOOR = 1e-6   # window energy, relative to the burst's, to score at all


def _ncc_delay(recording: np.ndarray, burst: np.ndarray) -> tuple[int, float]:
    """Best match position of `burst` in `recording`, and how good the match is.

    Normalised cross-correlation divides by window energy, so a near-silent
    window — an unpatched cable, the wrong input channel — can score high on
    nothing but numerical noise. Windows below an energy floor are excluded, and
    the caller is expected to reject a peak below NCC_MIN rather than report a
    latency that is really an artefact.
    """
    corr = np.correlate(recording, burst, mode="valid")
    burst_e = float(np.dot(burst, burst))
    c = np.concatenate([[0.0], np.cumsum(recording * recording)])
    L = len(burst)
    win_e = c[L:] - c[:-L]
    ncc = corr / (np.sqrt(win_e * burst_e) + _EPS)
    ncc[win_e < NCC_ENERGY_FLOOR * burst_e] = 0.0
    i = int(np.argmax(ncc))
    return i, float(ncc[i])


def cmd_devices(as_json: bool) -> int:
    hits = _scarlett_hits()
    rows, lines = [], []
    for i, d in enumerate(sd.query_devices()):
        is_sc = DEVICE_HINT.lower() in d["name"].lower()
        rows.append({
            "index": i, "name": d["name"],
            "max_input_channels": int(d["max_input_channels"]),
            "max_output_channels": int(d["max_output_channels"]),
            "default_samplerate": float(d["default_samplerate"]),
            "scarlett": is_sc,
        })
        mark = " <<<" if is_sc else ""
        lines.append(
            f"[{i:3d}] {d['name']!r}  in={d['max_input_channels']}  "
            f"out={d['max_output_channels']}  sr={d['default_samplerate']}{mark}"
        )
    result = DevicesResult(bool(hits), [i for i, _ in hits], rows)
    _emit(result, as_json, "\n".join(lines) + f"\nscarlett_found={bool(hits)}")
    if not hits:
        print(f"error: no device whose name contains {DEVICE_HINT!r}", file=sys.stderr)
        return 1
    return 0

def cmd_xruns(seconds: float, block: int, as_json: bool) -> int:
    device = _resolve_device(need_in=2, need_out=4)
    n_max = int(seconds * SR / block) + 8
    times_ms = np.zeros(n_max, dtype=np.float64)
    counts = np.zeros(3, dtype=np.int64)  # blocks, in_ovf, out_udf

    def callback(indata, outdata, frames, time_info, status):  # noqa: ARG001
        t0 = time.perf_counter()
        outdata.fill(0)
        i = int(counts[0])
        if status:
            if status.input_overflow:
                counts[1] += 1
            if status.output_underflow:
                counts[2] += 1
        if i < n_max:
            times_ms[i] = (time.perf_counter() - t0) * 1000.0
            counts[0] = i + 1

    gc.disable()
    try:
        with sd.Stream(
            device=device, samplerate=SR, blocksize=block, channels=(2, 4),
            dtype="float32", latency=LATENCY, callback=callback,
        ) as stream:
            lat = stream.latency
            time.sleep(seconds)
    finally:
        gc.enable()

    n = int(counts[0])
    used = times_ms[:n] if n else np.zeros(1)
    result = XrunsResult(
        n, int(counts[1]), int(counts[2]),
        float(used.max()) if n else 0.0, float(used.mean()) if n else 0.0,
        float(lat[0]) * 1000.0, float(lat[1]) * 1000.0,
        int(counts[1]) == 0 and int(counts[2]) == 0,
    )
    _emit(result, as_json, (
        f"blocks={result.blocks}  in_overflow={result.input_overflows}  "
        f"out_underflow={result.output_underflows}\n"
        f"callback_ms max={result.max_callback_ms:.3f} mean={result.mean_callback_ms:.3f}\n"
        f"latency_ms in={result.latency_in_ms:.2f} out={result.latency_out_ms:.2f}\n"
        f"ok={result.ok}"
    ))
    return 0 if result.ok else 1

def cmd_map(out_ch: int, confirmed: bool, as_json: bool) -> int:
    _require_amp_off(confirmed)
    n_out, n_in, seconds = out_ch + 1, 4, 5.0
    device = _resolve_device(need_in=n_in, need_out=n_out)
    phase = np.zeros(1, dtype=np.float64)
    latest = np.zeros(n_in, dtype=np.float64)
    omega = 2.0 * math.pi * TEST_TONE_HZ / SR
    history: list[list[float]] = []
    # Preallocated: the callback must not loop in Python or allocate.
    ramp = omega * np.arange(BLOCK, dtype=np.float64)
    scratch = np.zeros(BLOCK, dtype=np.float64)
    two_pi = 2.0 * math.pi

    def callback(indata, outdata, frames, time_info, status):  # noqa: ARG001
        outdata.fill(0)
        ph = float(phase[0])
        np.add(ramp[:frames], ph, out=scratch[:frames])
        np.sin(scratch[:frames], out=scratch[:frames])
        np.multiply(scratch[:frames], TEST_TONE_AMP, out=scratch[:frames])
        outdata[:frames, out_ch] = scratch[:frames]
        phase[0] = math.fmod(ph + omega * frames, two_pi)
        for ch in range(n_in):
            col = indata[:, ch]
            latest[ch] = math.sqrt(float(np.dot(col, col)) / frames)

    with sd.Stream(
        device=device, samplerate=SR, blocksize=BLOCK, channels=(n_in, n_out),
        dtype="float32", latency=LATENCY, callback=callback,
    ):
        t_end = time.time() + seconds
        while time.time() < t_end:
            time.sleep(1.0)
            row = [float(latest[c]) for c in range(n_in)]
            history.append(row)
            if not as_json:
                rms_s = "  ".join(f"in{c}={row[c]:.4f}" for c in range(n_in))
                print(f"t~{len(history)}s  out={out_ch}  {rms_s}", flush=True)

    result = MapResult(out_ch, seconds, history)
    _emit(result, as_json, f"map out={out_ch} seconds={seconds} samples={len(history)}")
    return 0

def cmd_meter(channel: int, seconds: float, as_json: bool) -> int:
    n_in = channel + 1
    device = _resolve_device(need_in=n_in, need_out=0)
    peak_abs = np.zeros(1, dtype=np.float64)
    latest_rms = np.zeros(1, dtype=np.float64)

    def callback(indata, frames, time_info, status):  # noqa: ARG001
        col = indata[:, channel]
        latest_rms[0] = math.sqrt(float(np.dot(col, col)) / frames)
        m = float(np.max(np.abs(col)))
        if m > peak_abs[0]:
            peak_abs[0] = m

    with sd.InputStream(
        device=device, samplerate=SR, blocksize=BLOCK, channels=n_in,
        dtype="float32", latency=LATENCY, callback=callback,
    ):
        t_end = time.time() + seconds
        while time.time() < t_end:
            time.sleep(0.1)
            rms_db, peak_db = _dbfs(float(latest_rms[0])), _dbfs(float(peak_abs[0]))
            n = int((max(-60.0, min(0.0, rms_db)) + 60.0) / 60.0 * 40)
            if not as_json:
                print(
                    f"ch={channel}  rms={rms_db:7.1f} dBFS  peak={peak_db:7.1f} dBFS  "
                    f"|{'#' * n}{'-' * (40 - n)}|",
                    flush=True,
                )

    peak_dbfs = _dbfs(float(peak_abs[0]))
    verdict = (
        "too_hot" if peak_dbfs > -9.0 else "too_quiet" if peak_dbfs < -18.0 else "ok"
    )
    result = MeterResult(channel, seconds, peak_dbfs, verdict)
    _emit(result, as_json, f"peak={peak_dbfs:.1f} dBFS  target=[-18,-9]  verdict={verdict}")
    return 0

def cmd_latency(out_ch: int, in_ch: int, confirmed: bool, as_json: bool) -> int:
    _require_amp_off(confirmed)
    n_out, n_in = out_ch + 1, in_ch + 1
    device = _resolve_device(need_in=n_in, need_out=n_out)
    burst = _make_burst()
    burst_n = len(burst)
    emit_at, capture_n = SR // 10, SR
    delays_samples: list[int] = []
    scores: list[float] = []
    misses = 0

    gc.disable()
    try:
        for trial in range(5):
            rec = np.zeros(capture_n, dtype=np.float32)
            write_pos = np.zeros(1, dtype=np.int64)
            read_pos = np.zeros(1, dtype=np.int64)

            def callback(indata, outdata, frames, time_info, status,
                         _rec=rec, _wp=write_pos, _rp=read_pos):  # noqa: ARG001
                outdata.fill(0)
                wp = int(_wp[0])
                # Overlap of this block with the burst's slot, as a slice. The burst
                # straddles a block boundary more often than not, so this is not an
                # all-or-nothing copy.
                lo = wp if wp > emit_at else emit_at
                hi = wp + frames if wp + frames < emit_at + burst_n else emit_at + burst_n
                if lo < hi:
                    outdata[lo - wp:hi - wp, out_ch] = burst[lo - emit_at:hi - emit_at]
                _wp[0] = wp + frames
                rp = int(_rp[0])
                n = min(frames, capture_n - rp)
                if n > 0:
                    _rec[rp:rp + n] = indata[:n, in_ch]
                    _rp[0] = rp + n

            with sd.Stream(
                device=device, samplerate=SR, blocksize=BLOCK, channels=(n_in, n_out),
                dtype="float32", latency=LATENCY, callback=callback,
            ):
                while int(read_pos[0]) < capture_n:
                    time.sleep(0.01)

            found, score = _ncc_delay(rec.astype(np.float64), burst.astype(np.float64))
            delay = found - emit_at
            if score < NCC_MIN or delay < 0:
                misses += 1
                if not as_json:
                    print(
                        f"trial {trial + 1}/5  NO BURST FOUND (score={score:.2f}) — "
                        f"is out {out_ch} patched to in {in_ch}?",
                        flush=True,
                    )
                continue
            delays_samples.append(int(delay))
            scores.append(score)
            if not as_json:
                print(
                    f"trial {trial + 1}/5  delay={delay} samples "
                    f"({delay / SR * 1000.0:.2f} ms)  score={score:.2f}",
                    flush=True,
                )
    finally:
        gc.enable()

    if len(delays_samples) < 3:
        print(
            f"error: only {len(delays_samples)}/5 trials found the burst. "
            "Not a latency measurement. Check the loopback cable and the channel "
            "indices before trusting any number from this tool.",
            file=sys.stderr,
        )
        return 1

    delays_ms = [d / SR * 1000.0 for d in delays_samples]
    med_s, med_ms = float(np.median(delays_samples)), float(np.median(delays_ms))
    spread = float(max(delays_ms) - min(delays_ms))
    result = LatencyResult(
        out_ch, in_ch, delays_samples, delays_ms, med_s, med_ms, spread, spread <= 1.0,
        len(delays_samples), min(scores),
    )
    if not result.spread_ok and not as_json:
        print(f"warning: delay spread {spread:.2f} ms exceeds 1 ms", file=sys.stderr)
    _emit(result, as_json, (
        f"median={med_s:.1f} samples ({med_ms:.2f} ms)  "
        f"spread={spread:.2f} ms  spread_ok={result.spread_ok}  "
        f"found={result.trials_found}/5  score_min={result.score_min:.2f}"
    ))
    return 0

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="rig.io_check", description=__doc__.split("\n\n")[0])
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="emit one JSON result object")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("devices", parents=[common], help="list devices; require Scarlett")
    px = sub.add_parser("xruns", parents=[common], help="duplex silence; fail on xrun")
    px.add_argument("--seconds", type=float, default=60.0)
    px.add_argument("--block", type=int, default=BLOCK)
    pm = sub.add_parser("map", parents=[common], help="tone on one out; watch in RMS")
    pm.add_argument("--out", type=int, required=True, dest="out_ch")
    pm.add_argument("--i-confirm-amp-is-off", action="store_true")
    pe = sub.add_parser("meter", parents=[common], help="input meter for gain setting")
    pe.add_argument("--channel", type=int, default=0)
    pe.add_argument("--seconds", type=float, default=20.0)
    pl = sub.add_parser("latency", parents=[common], help="cable loopback round-trip")
    pl.add_argument("--out", type=int, required=True, dest="out_ch")
    pl.add_argument("--in", type=int, required=True, dest="in_ch")
    pl.add_argument("--i-confirm-amp-is-off", action="store_true")

    args = p.parse_args(argv)
    j = bool(args.json)
    if args.cmd == "devices":
        return cmd_devices(j)
    if args.cmd == "xruns":
        return cmd_xruns(args.seconds, args.block, j)
    if args.cmd == "map":
        return cmd_map(args.out_ch, args.i_confirm_amp_is_off, j)
    if args.cmd == "meter":
        return cmd_meter(args.channel, args.seconds, j)
    if args.cmd == "latency":
        return cmd_latency(args.out_ch, args.in_ch, args.i_confirm_amp_is_off, j)
    return 2


if __name__ == "__main__":
    sys.exit(main())
