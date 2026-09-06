# Host harness

Runs on the Mac, not on the Bela.

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 -m pytest tests/
```

## What is here

- `harness/metrics.py` — the proxy metrics of `docs/ground-rules-and-facts.md` section 9,
  scoring a recorded take: how many partials are in play, how much the loudest one
  dominates, how flat the peak set is, whether the feedback stayed alive, how often the
  dominant mode jumps. These stand in for Abel's ears during automated tuning.
- `tests/` — synthetic-signal sanity checks. They cannot prove the metrics match his
  judgement; they prove the metrics point the right way on unambiguous cases.

## What is not here yet

- **pybela streaming** (Phase 1). `Watcher` on the Bela side, `Streamer` on this side,
  over a websocket — this is how the agent sees the DSP's internal state (detected peaks,
  per-cell frequency and gain reduction) without recompiling for every look.
- **The loop simulator** (Phase 2). The measured exciter-to-pickup impulse response, the
  loop delay and a saturating nonlinearity, closed around the DSP under test, run faster
  than real time. This is what makes unattended parameter search possible at all.
- **The log writer**. One record per run: commit hash, rig-profile hash, parameter set,
  metrics, path to the audio.
