"""
Phase 1 instrumentation smoke test: confirm pybela can stream Watcher variables
from the Bela Gem Stereo over the websocket link.

Companion to bela/watcher-check/render.cpp, which watches audio_in_ch0 /
audio_in_ch1. Deploy and run that project first, then run this.

Usage:
    python3 -m rig.watcher_check
"""
import numpy as np

# --- compat shim, 2026-09-11 ---
# The `pybela` package on PyPI (0.1.0) is stale relative to its own GitHub source
# (2.0.3): Watcher.py checks `ws.open`, a `websockets` <=12-era API. The
# `websockets` version pip actually resolves today (17.x) replaced that with a
# `.state` enum, so every connection check in pybela raises AttributeError and
# the whole thing hangs (each raised exception is swallowed inside an asyncio
# task and never surfaces). `pip install git+https://github.com/BelaPlatform/
# pybela.git` would pull the fixed source, but that install silently failed in
# this environment (network sandboxing around pip's git subprocess, as best as
# could be determined) -- so patch it at the class level instead, here, rather
# than hand-editing files inside .venv/site-packages that a reinstall would
# clobber anyway. Safe to delete once the PyPI package catches up.
import websockets.asyncio.client as _ws_client_mod
from websockets.protocol import State as _WsState
if not hasattr(_ws_client_mod.ClientConnection, "open"):
    _ws_client_mod.ClientConnection.open = property(
        lambda self: self.state == _WsState.OPEN
    )

from pybela import Streamer

VARIABLES = ["audio_in_ch0", "audio_in_ch1"]
N_VALUES = 2000  # a few blocks' worth at audio rate


def main():
    streamer = Streamer()  # defaults to ip=192.168.7.2, port=5555 -- matches this rig

    print(f"connecting to {streamer.ip}:{streamer.port} ...")
    streamer.connect()
    print(f"connected to project: {streamer.project_name}")
    print(f"watcher variables on board: {[v['name'] for v in streamer.watcher_vars]}")

    print(f"streaming {N_VALUES} values of {VARIABLES} ...")
    data = streamer.stream_n_values(variables=VARIABLES, n_values=N_VALUES)

    for var in VARIABLES:
        # This pybela version's stream_n_values() returns, per variable, a list
        # of buffer dicts {"ref_timestamp": ..., "data": [floats...]} rather
        # than a flat list of values -- confirmed by inspection, not assumed.
        values = np.concatenate([np.asarray(buf["data"], dtype=float) for buf in data[var]])
        n = len(values)
        peak = float(np.max(np.abs(values))) if n else float("nan")
        rms = float(np.sqrt(np.mean(values ** 2))) if n else float("nan")
        print(f"  {var}: {n} values, peak={peak:.4f}, rms={rms:.4f}")

    print("OK -- pybela/Watcher streaming confirmed working on this board.")
    # No disconnect() on this pybela version's Streamer/Watcher -- the process
    # exiting is enough; stream_n_values already stops the stream itself.


if __name__ == "__main__":
    main()
