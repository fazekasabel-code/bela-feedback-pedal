"""
Phase M1: close the controlled loop and find out whether it feeds back.

    guitar -> Scarlett in 1 -> [ master gain ] -> [ ceiling ] -> out 3/4 -> amp
                                                                            |
              pickup <- strings <- body <- exciter <---------------------- /

The DSP here is one number: a master gain. That is deliberate. M1 answers "does this rig
feed back, at what loop gain, and on which partial" — nothing else. The regulator arrives
in M3, and it will be judged against the recordings this makes.

Controls, live, from the keyboard:

    up / k    master gain +1 dB          space   mute (instant, latched)
    down / j  master gain -1 dB          u       unmute, from silence
    r         mark a take boundary       q       stop and write the recording

Safety, per docs/ground-rules-and-facts.md section 4.6 and CLAUDE.md:

  * CEILING is a hard constant. No tuning process may raise it. Human commit only.
  * Any non-finite sample in or out mutes the output for the rest of the run.
  * The run is time-boxed. It cannot outlive the session that started it.
  * Gain changes are ramped over a block, never stepped, or the step is a click and the
    loop amplifies it.

What this cannot do: control the physical level. That is Abel's hand on the headphone-2
knob. The software can only ever make things quieter than the amp is set to.
"""

from __future__ import annotations

import argparse
import gc
import json
import math
import queue
import select
import sys
import termios
import time
import tty
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path

import numpy as np
import sounddevice as sd

from .io_check import BLOCK, LATENCY, SR, _resolve_device

# --------------------------------------------------------------------- safety

# The hard output ceiling. A safety constant, not a parameter. Ground rule 1: no
# automatic tuning, parameter search or optimisation process may raise this, and it
# changes only by an explicit human commit. 0.7 is about -3 dBFS: high enough that it
# is not doing musical work in normal operation (if it is, the gain staging is wrong),
# low enough to leave the converter headroom.
CEILING = 0.7

# Beyond this the run stops whatever else is happening. Generous, because Abel has
# accepted sustained feedback on this rig (the guitar-amp path is dead, so the
# uncontrolled acoustic loop cannot form) — but finite, so a runaway can never outlive
# the session. Raise it with --seconds, not by editing this.
DEFAULT_TIMEBOX_S = 300.0

MIN_GAIN_DB = -80.0   # below this we simply mute; -80 dB is silence, not a quiet signal
MAX_GAIN_DB = 12.0    # a ceiling on the *software* gain, distinct from CEILING


# -------------------------------------------------------------------- results

@dataclass
class RunResult:
    run_name: str
    seconds: float
    blocks: int
    sample_rate: int
    block_size: int
    input_channel: int
    output_channels: list
    ceiling: float
    final_gain_db: float
    gain_events: list = field(default_factory=list)  # (t_seconds, gain_db)
    take_marks: list = field(default_factory=list)   # t_seconds
    input_peak_dbfs: float = -200.0
    output_peak_dbfs: float = -200.0
    ceiling_hits: int = 0
    input_overflows: int = 0
    output_underflows: int = 0
    muted_by_fault: bool = False
    fault_reason: str = ""
    timeboxed: bool = False
    audio_path: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


# -------------------------------------------------------------------- helpers

def _dbfs(x: float) -> float:
    return 20.0 * math.log10(max(abs(x), 1e-12))


def _db_to_lin(db: float) -> float:
    return 0.0 if db <= MIN_GAIN_DB else 10.0 ** (db / 20.0)


def apply_gain_and_ceiling(
    src: np.ndarray, g0: float, g1: float, dst: np.ndarray, ramp: np.ndarray
) -> int:
    """Ramp the gain from g0 to g1 across the block, then clip to CEILING.

    Returns how many samples the ceiling caught. Pulled out of the callback so it can
    be tested: this is the one piece of arithmetic standing between a runaway loop and
    the exciter, and "I read it and it looked right" is not good enough for that.

    Allocation-free: `dst` and `ramp` are caller-owned scratch, at least len(src) long.
    """
    n = len(src)
    if g0 != g1:
        np.multiply(np.arange(n, dtype=np.float32), (g1 - g0) / n, out=ramp[:n])
        np.add(ramp[:n], g0, out=ramp[:n])
        np.multiply(src, ramp[:n], out=dst[:n])
    else:
        np.multiply(src, g0, out=dst[:n])

    hits = int(np.count_nonzero(np.abs(dst[:n]) > CEILING))
    if hits:
        np.clip(dst[:n], -CEILING, CEILING, out=dst[:n])
    return hits


class _RawKeys:
    """Non-blocking single-key reads from a tty, restored on the way out.

    Falls back to doing nothing when stdin is not a tty (piped, or under a harness),
    so the same script still runs unattended with --ramp.
    """

    def __init__(self) -> None:
        self.enabled = sys.stdin.isatty()
        self._saved = None

    def __enter__(self) -> "_RawKeys":
        if self.enabled:
            self._saved = termios.tcgetattr(sys.stdin)
            tty.setcbreak(sys.stdin.fileno())
        return self

    def __exit__(self, *exc) -> None:
        if self.enabled and self._saved is not None:
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, self._saved)

    def get(self) -> str | None:
        if not self.enabled:
            return None
        if select.select([sys.stdin], [], [], 0)[0]:
            ch = sys.stdin.read(1)
            if ch == "\x1b":  # arrow keys arrive as an escape sequence
                if select.select([sys.stdin], [], [], 0.01)[0]:
                    rest = sys.stdin.read(2)
                    return {"[A": "up", "[B": "down"}.get(rest, "")
            return ch
        return None


# ------------------------------------------------------------------ the run

def run(
    run_name: str,
    seconds: float,
    input_channel: int,
    output_channels: list[int],
    start_gain_db: float,
    ramp_to_db: float | None,
    ramp_seconds: float,
    record_dir: Path,
) -> RunResult:
    n_out = max(output_channels) + 1
    n_in = input_channel + 1
    device = _resolve_device(need_in=n_in, need_out=n_out)

    # Everything the callback touches is allocated here and never again.
    capacity = int(seconds * SR) + SR
    rec_in = np.zeros(capacity, dtype=np.float32)
    rec_out = np.zeros(capacity, dtype=np.float32)
    scratch = np.zeros(BLOCK, dtype=np.float32)
    gain_ramp = np.zeros(BLOCK, dtype=np.float32)

    # Shared scalars, as arrays so the callback can write them without rebinding.
    cur_gain = np.array([_db_to_lin(start_gain_db)], dtype=np.float64)
    tgt_gain = np.array([_db_to_lin(start_gain_db)], dtype=np.float64)
    counters = np.zeros(6, dtype=np.int64)  # blocks, rec_n, ceil_hits, in_ovf, out_udf, fault
    peaks = np.zeros(2, dtype=np.float64)   # in, out
    fault_msg: queue.Queue[str] = queue.Queue()

    def callback(indata, outdata, frames, time_info, status):  # noqa: ARG001
        outdata.fill(0.0)
        if status:
            if status.input_overflow:
                counters[3] += 1
            if status.output_underflow:
                counters[4] += 1

        # Once faulted, this run stays silent. Ground rule 4: do not try to recover.
        if counters[5]:
            counters[0] += 1
            return

        src = indata[:frames, input_channel]

        # A non-finite sample is either a driver fault or a bug. Either way the loop is
        # about to be fed something meaningless, so mute and stay muted.
        if not np.all(np.isfinite(src)):
            counters[5] = 1
            fault_msg.put("non-finite sample on input")
            return

        # Ramp the gain across the block instead of stepping it (a stepped gain is a
        # click, and a click inside a feedback loop comes back louder), then clip to
        # the ceiling. Counted: if the ceiling is ever busy in normal running, the gain
        # staging is wrong, and that is a finding rather than something to absorb.
        g0, g1 = float(cur_gain[0]), float(tgt_gain[0])
        hits = apply_gain_and_ceiling(src, g0, g1, scratch, gain_ramp)
        cur_gain[0] = g1
        counters[2] += hits

        if not np.all(np.isfinite(scratch[:frames])):
            counters[5] = 1
            fault_msg.put("non-finite sample on output")
            return

        for ch in output_channels:
            outdata[:frames, ch] = scratch[:frames]

        ip = float(np.max(np.abs(src)))
        op = float(np.max(np.abs(scratch[:frames])))
        if ip > peaks[0]:
            peaks[0] = ip
        if op > peaks[1]:
            peaks[1] = op

        n = int(counters[1])
        room = min(frames, capacity - n)
        if room > 0:
            rec_in[n:n + room] = src[:room]
            rec_out[n:n + room] = scratch[:room]
            counters[1] = n + room
        counters[0] += 1

    result = RunResult(
        run_name=run_name, seconds=0.0, blocks=0, sample_rate=SR, block_size=BLOCK,
        input_channel=input_channel, output_channels=list(output_channels),
        ceiling=CEILING, final_gain_db=start_gain_db,
    )
    gain_db = start_gain_db
    result.gain_events.append((0.0, gain_db))

    print(f"ceiling={CEILING}  time box={seconds:.0f}s  "
          f"in={input_channel} out={output_channels}")
    print("keys:  up/k +1dB   down/j -1dB   space mute   u unmute   r mark   q quit")
    print(f"starting at {gain_db:+.0f} dB. Bring the amp up slowly.\n")

    t0 = time.monotonic()
    timeboxed = False
    gc.disable()
    try:
        with _RawKeys() as keys, sd.Stream(
            device=device, samplerate=SR, blocksize=BLOCK, channels=(n_in, n_out),
            dtype="float32", latency=LATENCY, callback=callback,
        ):
            last_print = 0.0
            while True:
                now = time.monotonic() - t0
                if now >= seconds:
                    timeboxed = True
                    break
                if counters[5]:
                    break

                k = keys.get()
                if k:
                    if k in ("q", "\x03"):
                        break
                    elif k in ("up", "k"):
                        gain_db = min(MAX_GAIN_DB, gain_db + 1.0)
                    elif k in ("down", "j"):
                        gain_db = max(MIN_GAIN_DB, gain_db - 1.0)
                    elif k == " ":
                        gain_db = MIN_GAIN_DB
                    elif k == "u":
                        gain_db = -40.0
                    elif k == "r":
                        result.take_marks.append(round(now, 3))
                        print(f"\n-- mark at {now:.1f}s --")
                    if k in ("up", "k", "down", "j", " ", "u"):
                        tgt_gain[0] = _db_to_lin(gain_db)
                        result.gain_events.append((round(now, 3), gain_db))

                # Unattended slow ramp, for runs with no one at the keyboard.
                if ramp_to_db is not None and now < ramp_seconds:
                    frac = now / ramp_seconds
                    gain_db = start_gain_db + frac * (ramp_to_db - start_gain_db)
                    tgt_gain[0] = _db_to_lin(gain_db)

                if now - last_print >= 0.25:
                    last_print = now
                    ip, op = _dbfs(peaks[0]), _dbfs(peaks[1])
                    print(f"\r{now:6.1f}s  gain={gain_db:+6.1f} dB  "
                          f"in_pk={ip:6.1f}  out_pk={op:6.1f}  "
                          f"ceil_hits={int(counters[2])}   ", end="", flush=True)
                time.sleep(0.01)
    finally:
        gc.enable()

    elapsed = time.monotonic() - t0
    print()

    result.seconds = round(elapsed, 3)
    result.blocks = int(counters[0])
    result.final_gain_db = gain_db
    result.input_peak_dbfs = round(_dbfs(peaks[0]), 2)
    result.output_peak_dbfs = round(_dbfs(peaks[1]), 2)
    result.ceiling_hits = int(counters[2])
    result.input_overflows = int(counters[3])
    result.output_underflows = int(counters[4])
    result.timeboxed = timeboxed
    if counters[5]:
        result.muted_by_fault = True
        try:
            result.fault_reason = fault_msg.get_nowait()
        except queue.Empty:
            result.fault_reason = "unknown"

    n = int(counters[1])
    if n:
        record_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%H%M%S")
        path = record_dir / f"{stamp}_{run_name}.wav"
        stereo = np.stack([rec_in[:n], rec_out[:n]], axis=1)
        _write_wav(path, stereo, SR)
        result.audio_path = str(path)
        print(f"wrote {path}  ({n / SR:.1f}s, ch0=input ch1=output)")

    return result


def _write_wav(path: Path, data: np.ndarray, sr: int) -> None:
    """16-bit stereo WAV via the stdlib, so this does not depend on soundfile."""
    import wave

    clipped = np.clip(data, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


# ------------------------------------------------------------------------ CLI

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="rig.loop", description="M1: close the controlled loop and confirm feedback."
    )
    p.add_argument("--name", default="m1-loop", help="run name, used in the filenames")
    p.add_argument("--seconds", type=float, default=DEFAULT_TIMEBOX_S,
                   help=f"time box (default {DEFAULT_TIMEBOX_S:.0f})")
    p.add_argument("--in-channel", type=int, default=0)
    p.add_argument("--out-channels", type=int, nargs="+", default=[2, 3])
    p.add_argument("--gain-db", type=float, default=-40.0,
                   help="starting master gain (default -40, i.e. very quiet)")
    p.add_argument("--ramp-to-db", type=float, default=None,
                   help="unattended: ramp the gain to this over --ramp-seconds")
    p.add_argument("--ramp-seconds", type=float, default=30.0)
    p.add_argument("--logs", type=Path, default=None, help="where to write the recording")
    p.add_argument("--json", action="store_true")
    p.add_argument("--i-confirm-amp-is-live", action="store_true",
                   help="required: Abel is present with the headphone-2 knob in reach")
    args = p.parse_args(argv)

    if not args.i_confirm_amp_is_live:
        print(
            "error: this closes the feedback loop through the exciter and refuses to run "
            "without --i-confirm-amp-is-live.\n"
            "Before passing it: the guitar amp is dead (only the controlled loop is live), "
            "the headphone-2 level knob is in reach, and the amp is turned down.",
            file=sys.stderr,
        )
        return 1
    if args.gain_db > 0.0:
        print(f"error: refusing to start at {args.gain_db:+.0f} dB. Start quiet and come up.",
              file=sys.stderr)
        return 1

    root = Path(__file__).resolve().parents[2]
    logs = args.logs or root / "logs" / datetime.now().strftime("%Y-%m-%d")

    result = run(
        run_name=args.name, seconds=args.seconds, input_channel=args.in_channel,
        output_channels=args.out_channels, start_gain_db=args.gain_db,
        ramp_to_db=args.ramp_to_db, ramp_seconds=args.ramp_seconds, record_dir=logs,
    )

    # Ground rule 11: a result without its parameter set is not a result. This is not
    # optional and not something to remember to do — every run writes its record.
    try:
        from harness.logrecord import write_record

        rec_path = write_record(
            run_name=args.name,
            params={
                "phase": "M1", "sample_rate": SR, "block_size": BLOCK,
                "ceiling": CEILING, "latency_hint": LATENCY,
                "input_channel": args.in_channel, "output_channels": args.out_channels,
                "start_gain_db": args.gain_db, "final_gain_db": result.final_gain_db,
                "ramp_to_db": args.ramp_to_db, "ramp_seconds": args.ramp_seconds,
                "timebox_s": args.seconds,
                "gain_events": result.gain_events, "take_marks": result.take_marks,
            },
            metrics=None,  # scored later from the wav, by harness.metrics
            audio_path=result.audio_path or "(no audio)",
            notes=(f"muted by fault: {result.fault_reason}" if result.muted_by_fault
                   else ""),
        )
        print(f"log record: {rec_path}")
    except Exception as exc:  # noqa: BLE001 - a logging failure must not lose the audio
        print(f"warning: could not write the log record: {exc}", file=sys.stderr)

    if args.json:
        print(json.dumps(result.to_dict(), indent=2, default=str))
    else:
        print(f"\nblocks={result.blocks}  in_pk={result.input_peak_dbfs} dBFS  "
              f"out_pk={result.output_peak_dbfs} dBFS")
        print(f"ceiling hits={result.ceiling_hits}  xruns="
              f"{result.input_overflows}/{result.output_underflows}  "
              f"final gain={result.final_gain_db:+.1f} dB")
        if result.muted_by_fault:
            print(f"MUTED BY FAULT: {result.fault_reason}")
        if result.ceiling_hits:
            print("note: the ceiling was active. That is a gain-staging finding, not a "
                  "detail — the safety limiter should not be doing musical work.")
    return 2 if result.muted_by_fault else 0


if __name__ == "__main__":
    sys.exit(main())
