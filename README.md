# bela-feedback-pedal

Gen1 DSP feedback regulator for guitar.

**Goal:** suppress the winner-takes-all character of guitar feedback and produce rich,
complex, multi-partial feedback tones.

**Platform: Bela Gem Stereo** (Starter Kit, PocketBeagle 2 base). Live plan:
[`docs/phase-plan.md`](docs/phase-plan.md). Hardware specifics — and which of them are
still unverified on Abel's unit — are in
[`docs/ground-rules-and-facts.md`](docs/ground-rules-and-facts.md) §3.1.

Read [`docs/ground-rules-and-facts.md`](docs/ground-rules-and-facts.md) before touching
anything. It holds the signal topology, the hardware facts, the electrical gotchas, the
DSP architecture and the safety rules. [`docs/phase-plan.md`](docs/phase-plan.md) holds the
sequence of work.

## Status

**On the Bela Gem Stereo, arrived 2026-09-10. Bring-up under way.** Signal chain as wired,
2026-09-11: Epiphone (humbucker) → M4 (buffered direct out) → Bela in; Bela out → power amp
(Dayton DTA120) → exciter (Dayton), straight through, no pedal in between — see
ground-rules §4.5 for why this build carries no dedicated hardware kill switch. A one-cell
adaptive regulator has already passed the "second partial blooms" test; that code and the
harness carry over.

| Piece | State |
|---|---|
| `docs/phase-plan.md` | Live plan. Adjust for the Gem's specifics as you go |
| `docs/ground-rules-and-facts.md` | Current. Physics/DSP/metrics unchanged; see §3.1 for the Gem hardware, §4.5 for the fail-safe decision |
| `sc/gen1_cell.scd` | One adaptive cell, passed the "second partial blooms" test. Detector needs replacing (FFT peak picking, not pitch tracking) |
| `host/harness/metrics.py` | Implemented, run against real recorded audio already |
| `host/rig/` | I/O bring-up + loop runner; needs a Bela-Gem equivalent |
| `rig-profile.json` | v2, platform `bela-gem-stereo`; input signal confirmed 2026-09-11, most else still `null` |
| `bela/gen1-passthrough/` | Skeleton, **never compiled**. Check against the current IDE |
| `bela/io-check-input/` | Output-silent input-level diagnostic. Confirmed the signal chain end to end, 2026-09-11 |
| `scripts/deploy.sh` | Wraps `build_project.sh`; SSH bring-up confirmed, `deploy.sh`'s own host-side path still unverified |

## Layout

```
docs/                       ground rules, facts, phase plan
bela/gen1-passthrough/      Phase 0 hello-world: passthrough + output ceiling + mute-on-error
host/harness/               host-side Python: proxy metrics, rig profile, pybela streaming
scripts/deploy.sh           wrapper around Bela's build_project.sh
rig-profile.json            the measured truth about this rig — everything reads it
logs/                       one record per test run (gitignored except .gitkeep and .json records)
```

## Non-negotiables

1. **No live guitar amp or PA anywhere in the signal path.** One doesn't exist in this
   build at all — the guitar goes only into the M4 → Bela — and it stays that way, because
   a live amp would close a second, uncontrolled acoustic feedback loop that fights the
   regulator (and rule 3 below stops holding if it's ever added).
2. The output ceiling in the DSP is a safety constant. **No automatic tuning process
   may raise it.**
3. Real-loop tests happen only with Abel present. **This build has no dedicated hardware
   kill switch** — Bela audio out feeds the Dayton DTA120 straight into the exciter, no
   pedal in between — by deliberate decision, not by omission: see ground-rules §4.5.
4. No monitoring mix or hardware passthrough may feed the audio input back to the output
   outside the DSP — that closes an analog loop the DSP cannot see or mute.
5. The split feeding the board must be **buffered** — satisfied by the M4's buffered
   instrument input regardless of what the Gem's own input impedance turns out to need
   (ground-rules §3.1, §4.1).
