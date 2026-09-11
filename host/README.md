# Host harness

Runs on the Mac, not on the Bela.

**Put the venv outside iCloud Drive.** This repo lives under `~/Library/Mobile
Documents/com~apple~CloudDocs/...`, and a `.venv` created *inside* it (e.g. plain
`python3 -m venv .venv` here) hits random multi-second-to-a-minute stalls importing
numpy/scipy/soundfile/pybela — discovered 2026-09-11 chasing exactly that. Cause as
best determined: iCloud's sync/indexing daemon intermittently locks newly-written
native `.so`/`.dylib` files the first time each is opened. It's not deterministic
which import stalls or for how long, which makes it easy to mistake for a real hang.
Fix: keep the venv itself somewhere local.

```bash
python3.11 -m venv ~/.venvs/bela-feedback-pedal   # <=3.12: pybela's jupyter-bokeh
~/.venvs/bela-feedback-pedal/bin/pip install -r requirements.txt  # dep doesn't build on 3.13
~/.venvs/bela-feedback-pedal/bin/python -m pytest tests/
```

Run `rig.*` modules the same way, from `host/`:
`~/.venvs/bela-feedback-pedal/bin/python -m rig.harness_run ...`

## What is here

- `harness/metrics.py` — the proxy metrics of `docs/ground-rules-and-facts.md` section 9,
  scoring a recorded take: how many partials are in play, how much the loudest one
  dominates, how flat the peak set is, whether the feedback stayed alive, how often the
  dominant mode jumps. These stand in for Abel's ears during automated tuning.
- `harness/logrecord.py` — one JSON record per run under `logs/`: commit hash, rig-profile
  hash, parameter set, metrics, path to the audio. A result without its parameter set is
  not a result (CLAUDE.md rule 11).
- `rig/watcher_check.py` — Phase 1 smoke test confirming pybela/Watcher streaming works
  against a board running `bela/watcher-check/`. See ground-rules-and-facts.md §3.1.
- `rig/harness_run.py` — deploys `bela/harness-passthrough/` to the board, runs it for a
  time box, fetches the recording, scores it with `harness/metrics.py`, and writes a
  record with `harness/logrecord.py`. This drives the exciter for real — see its own
  docstring and ground rule 2.
- `tests/` — synthetic-signal sanity checks. They cannot prove the metrics match his
  judgement; they prove the metrics point the right way on unambiguous cases.

## What is not here yet

- **The loop simulator** (Phase 2). The measured exciter-to-pickup impulse response, the
  loop delay and a saturating nonlinearity, closed around the DSP under test, run faster
  than real time. This is what makes unattended parameter search possible at all.
- Live Watcher monitoring (in_peak/out_peak/muted) *during* a `harness_run.py` run, rather
  than only reading back the recording afterward — the Bela project already exposes those
  variables, wiring a live view into the host script is still open.
