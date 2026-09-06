# bela-feedback-pedal

Gen1 DSP feedback regulator for guitar, on Bela Cape + BeagleBone Black.

**Goal:** suppress the winner-takes-all character of guitar feedback and produce rich,
complex, multi-partial feedback tones.

Read [`docs/ground-rules-and-facts.md`](docs/ground-rules-and-facts.md) before touching
anything. It holds the signal topology, the verified hardware facts, the electrical
gotchas, the DSP architecture and the safety rules. [`docs/phase-plan.md`](docs/phase-plan.md)
holds the sequence of work and who is needed for each phase.

## Status

**Phase 0 — bring-up. Nothing here has run on hardware yet.**

| Piece | State |
|---|---|
| `docs/` | Written, current |
| `bela/gen1-passthrough/` | Skeleton, **never compiled or run** |
| `host/harness/metrics.py` | Implemented, tested on synthetic signals only |
| `rig-profile.json` | Template, all values `null` — filled in Phase 1 |
| `scripts/deploy.sh` | Untested — needs a Bela on the network |

## Layout

```
docs/                       ground rules, facts, phase plan
bela/gen1-passthrough/      Phase 0 hello-world: passthrough + output ceiling + mute-on-error
host/harness/               host-side Python: proxy metrics, rig profile, pybela streaming
scripts/deploy.sh           wrapper around Bela's build_project.sh
rig-profile.json            the measured truth about this rig — everything reads it
logs/                       one record per test run (gitignored except .gitkeep)
```

## Quick start (once the Bela is on the network)

```bash
export BELA_SCRIPTS=~/Bela/scripts      # where Bela's build scripts live
export BBB_HOSTNAME=192.168.7.2         # or bela.local, or the LAN IP

./scripts/deploy.sh bela/gen1-passthrough gen1-passthrough
```

## Non-negotiables

1. The split feeding Bela is **buffered**. Bela's audio input is ~20 kΩ at the ADC —
   a passive pickup driving that directly loses level and high end.
2. The **amp stays silent** during development. It closes a second, uncontrolled
   acoustic feedback loop that will fight the regulator.
3. The output ceiling in the DSP is a safety constant. **No automatic tuning process
   may raise it.**
4. Real-loop tests happen only with Abel present and the volume pedal in reach.
