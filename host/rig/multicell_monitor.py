"""
Live terminal monitor for bela/gen1-multicell-live/, via pybela -- built 2026-09-13
as a workaround for Bela's own browser GUI (the WSServer-based live-plot view in the
IDE), which fails to connect from every real browser tried (Edge, Safari, and this
repo's own headless browser tool): a WebSocket error immediately after the handshake
(code 1006). This got a genuinely thorough investigation, not a shrug -- three
theories chased and ruled out with real evidence:

  - Watcher needing Bela's `dev` branch (this board is on `master`, confirmed via
    `git rev-parse --abbrev-ref HEAD`, and pybela's own README does say `watcher`
    is `dev`-only) -- true, but doesn't explain it: Bela's own core `Scope`
    (bela/gen1-multicell-live/render.cpp), a completely separate mechanism with
    nothing to do with Watcher or branches, fails the identical way.
  - Chrome's "Local Network Access" policy blocking a plain-HTTP page's WebSocket
    to a local address -- a strong fit, until Safari (which doesn't enforce that
    policy) failed the exact same way.
  - A non-standard HTTP reason phrase in the handshake ("101 WebSocket Protocol
    Handshake" instead of "101 Switching Protocols") -- the WHATWG spec only gates
    on the numeric 101 status, not the phrase text, so this shouldn't matter and
    doesn't appear to.
  - Also missing, found, and fixed along the way (real, but not sufficient on its
    own): Watcher's upstream repo ships a `sketch.js` the browser GUI needs to
    render anything at all, never vendored into any project here until now (see
    bela/gen1-multicell-live/sketch.js). Turns out the placeholder it was missing
    without that file is the SAME placeholder shown whenever the underlying
    connection hasn't come up -- so this closed a real gap without fixing the
    actual blank-page symptom.

What IS confirmed: Python's `websockets` library (via pybela) completes full,
sustained, correctly-parsed sessions against this exact board, so the server's own
WebSocket implementation isn't fundamentally broken -- every browser engine tried
fails at the same point regardless. No further lead without packet-level comparison
tools this environment doesn't have. Not chased further (Abel's call, 2026-09-14).

Getting this far surfaced a second, real, FIXED bug along the way: pybela's own
streaming (confirmed working in principle since 2026-09-11, host/rig/watcher_check.py)
initially failed too, with `'NoneType' object has no attribute 'groups'` from inside
pybela's own websocket handling, on every project of ours except watcher-check --
traced to this repo's projects writing their Watcher<T> telemetry only once per
render() block, while watcher-check (and, it turns out, pybela's own streaming
protocol) expects every watched variable written every audio sample. Fixed at the
source in bela/gen1-multicell/render.cpp and bela/gen1-multicell-live/render.cpp (see
that commit) -- this script is just the client for it, once that fix is in place.

Usage (the project must already be running on the board -- launch it from the Bela
IDE first, same as always):

    cd host && python3 -m rig.multicell_monitor
    cd host && python3 -m rig.multicell_monitor --cells 0,1,2,3 --refresh-s 0.5
"""
from __future__ import annotations

import argparse
import sys
import time

# --- compat shim, 2026-09-11 -- see host/rig/watcher_check.py's header for the
# full story. Safe to delete once the PyPI pybela package catches up.
import websockets.asyncio.client as _ws_client_mod
from websockets.protocol import State as _WsState
if not hasattr(_ws_client_mod.ClientConnection, "open"):
    _ws_client_mod.ClientConnection.open = property(
        lambda self: self.state == _WsState.OPEN
    )

from pybela import Streamer

GLOBAL_VARS = [
    "cells_bound_count", "in_peak", "out_peak", "muted",
    "bind_events_total", "release_events_total", "steal_events_total",
    "audio_thread_cpu_percent",
]


def cell_vars(cells: list[int]) -> list[str]:
    out = []
    for c in cells:
        out += [f"cell{c}_bound", f"cell{c}_freq_hz", f"cell{c}_cut_db", f"cell{c}_partial_db"]
    return out


def fmt_row(values: dict) -> str:
    def g(name, fmt="{:.2f}"):
        v = values.get(name)
        return fmt.format(v) if isinstance(v, (int, float)) else "?"

    header = (
        f"bound={g('cells_bound_count', '{:.0f}')}/12  "
        f"in={g('in_peak')}  out={g('out_peak')}  "
        f"muted={g('muted', '{:.0f}')}  cpu={g('audio_thread_cpu_percent', '{:.1f}')}%  "
        f"bind/rel/steal={g('bind_events_total', '{:.0f}')}/"
        f"{g('release_events_total', '{:.0f}')}/{g('steal_events_total', '{:.0f}')}"
    )
    return header


def fmt_cells(values: dict, cells: list[int]) -> str:
    lines = []
    for c in cells:
        bound = values.get(f"cell{c}_bound")
        if not bound:
            continue
        freq = values.get(f"cell{c}_freq_hz")
        cut = values.get(f"cell{c}_cut_db")
        lvl = values.get(f"cell{c}_partial_db")
        lines.append(f"  cell{c:>2}: {freq:7.1f} Hz  cut={cut:5.1f} dB  level={lvl:6.1f} dBFS")
    return "\n".join(lines) if lines else "  (no cells bound)"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="rig.multicell_monitor",
        description="Live terminal view of gen1-multicell-live's Watcher variables via pybela.",
    )
    p.add_argument("--cells", default="0,1,2,3,4,5,6,7,8,9,10,11",
                   help="comma-separated cell indices to show detail for (default: all 12)")
    p.add_argument("--refresh-s", type=float, default=0.5,
                   help="terminal redraw interval in seconds (default 0.5) -- each redraw is "
                        "its own stream_n_values() call; don't push this much below ~0.3s")
    args = p.parse_args(argv)

    cells = [int(c) for c in args.cells.split(",") if c.strip() != ""]
    variables = GLOBAL_VARS + cell_vars(cells)

    # Streamer, not Monitor: Monitor.peek() hits a bug in this pybela version's
    # connection handling ('NoneType' object has no attribute 'groups', inside
    # its own websocket reconnect logic) every time it opens the brief per-peek
    # session it needs. stream_n_values() is the mechanism host/rig/watcher_check.py
    # already confirmed working end to end (2026-09-11) -- reused here instead of
    # chasing a second pybela bug on top of the Bela-side Gui one this script
    # already exists to work around.
    streamer = Streamer()
    print(f"connecting to {streamer.ip}:{streamer.port} ...")
    streamer.connect()
    print(f"connected to project: {streamer.project_name}")

    on_board = {v["name"] for v in streamer.watcher_vars}
    missing = [v for v in variables if v not in on_board]
    if missing:
        print(f"warning: not on the board's watcher, skipping: {missing}", file=sys.stderr)
        variables = [v for v in variables if v in on_board]

    print("monitoring -- ctrl-C to stop\n")

    try:
        while True:
            data = streamer.stream_n_values(variables=variables, n_values=1)
            values = {}
            for name in variables:
                bufs = data.get(name) or []
                if bufs and bufs[-1].get("data"):
                    values[name] = bufs[-1]["data"][-1]
            print(fmt_row(values))
            cell_lines = fmt_cells(values, cells)
            if cell_lines:
                print(cell_lines)
            print("-" * 60)
            time.sleep(args.refresh_s)
    except KeyboardInterrupt:
        pass
    finally:
        print("stopped.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
