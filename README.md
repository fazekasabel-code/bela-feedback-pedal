# bela-feedback-pedal

Gen1 DSP feedback regulator for guitar.

**Goal:** suppress the winner-takes-all character of guitar feedback and produce rich,
complex, multi-partial feedback tones.

> **Platform, as of 2026-09-06: Mac + Focusrite Scarlett 8i6, not Bela.** The board is not
> connectable, so gen1 is being built on the Mac first — guitar into input 1 (INST), exciter
> chain off outputs 3/4, DSP in Python in the audio callback. The physics, the DSP
> architecture and the metrics are unchanged by the move; the Bela sources stay in `bela/`
> for when the board comes back. The live plan is
> [`docs/mac-rig-plan.md`](docs/mac-rig-plan.md).

Read [`docs/ground-rules-and-facts.md`](docs/ground-rules-and-facts.md) before touching
anything. It holds the signal topology, the verified hardware facts, the electrical
gotchas, the DSP architecture and the safety rules — see §3.3 and §4.6 for this rig.
[`docs/mac-rig-plan.md`](docs/mac-rig-plan.md) holds the sequence of work;
[`docs/phase-plan.md`](docs/phase-plan.md) is the parked Bela plan.

## Status

**Phase M0 — I/O bring-up on the Mac rig. Nothing here has run on hardware yet.**

| Piece | State |
|---|---|
| `docs/` | Written, current |
| `host/rig/io_check.py` | M0 bring-up tool — device map, xruns, levels, latency. Amp stays off |
| `host/harness/metrics.py` | Implemented, tested on synthetic signals only |
| `rig-profile.json` | Template, all values `null` — M0 and M1 fill it in |
| `bela/gen1-passthrough/` | Parked. Skeleton, **never compiled or run** |
| `scripts/deploy.sh` | Parked. Needs a Bela on the network |

## Layout

```
docs/                       ground rules, facts, phase plan
bela/gen1-passthrough/      Phase 0 hello-world: passthrough + output ceiling + mute-on-error
host/harness/               host-side Python: proxy metrics, rig profile, pybela streaming
scripts/deploy.sh           wrapper around Bela's build_project.sh
rig-profile.json            the measured truth about this rig — everything reads it
logs/                       one record per test run (gitignored except .gitkeep)
```

## Quick start

```bash
cd host
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python3 -m rig.io_check devices     # find the Scarlett, confirm the channel counts
python3 -m rig.io_check xruns       # 60 s at 128 frames, must come back clean
python3 -m rig.io_check meter       # guitar plugged in: aim for about -12 dBFS peak
```

The power amp stays **off** for all of the above.

## Non-negotiables

1. The **guitar amp stays silent** during development. It closes a second, uncontrolled
   acoustic feedback loop that will fight the regulator.
2. The output ceiling in the DSP is a safety constant. **No automatic tuning process
   may raise it.**
3. Real-loop tests happen only with Abel present and a hardware kill in reach. On this rig
   the Scarlett's monitor knob is **not** that kill — outputs 3/4 are fixed-level. The kill
   is a volume pedal between out 3/4 and the amp, or the amp's own volume control.
4. In Focusrite Control, outputs 3/4 are fed from **Playback 3/4 only**. A hardware mix
   carrying Input 1 to those outputs closes an analog loop the DSP cannot see or mute.
5. On the Bela, the split feeding the board must be **buffered** (~20 kΩ ADC input). The
   Scarlett's INST input handles this in hardware, so it only applies when the board returns.
