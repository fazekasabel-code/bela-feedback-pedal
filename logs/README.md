# logs/

One record per test run: git commit hash, rig-profile hash, full parameter set, all proxy
metrics, path to recorded audio. A result without its parameter set is not a result
(CLAUDE.md rule 11).

## Which rig profile each run belongs to

Per CLAUDE.md rule 13, when the rig changes, prior results are tagged as belonging to the
old profile. Each record already carries `provenance.rig_profile_sha256`, so the binding is
exact — this note is the human-readable version.

| Directory | Rig profile | Notes |
|---|---|---|
| `2026-09-06/` | v1 (earlier platform, see git history for its details) | M1 baselines + M3 one-cell regulator takes, measured in SuperCollider (`sc/gen1_cell.scd`). The "second partial blooms" pass and the detector defect it exposed live here. |

Runs from **2026-09-10 onward** are on the **Bela Gem Stereo** (`bela-gem-stereo`, profile
v2). They are not comparable to the `2026-09-06/` takes as absolute numbers — different
platform, different converters, different latency — but the qualitative targets (winner-
takes-all in the baseline; a second partial blooming with one cell) carry across.

**2026-09-13: profile bumped v2 → v3.** The M4 buffer/preamp was removed from the signal
chain (confirmed source of a ground loop, see `rig-profile.json`'s `gain_staging`
`_ground_loop_finding_2026-09-12` note and `docs/ground-rules-and-facts.md` §4.4) — the
guitar now runs direct/unbuffered into Bela's audio in. Everything under `2026-09-12/`
belongs to the **v2** M4-based chain and is retired, not just distrusted (the feedback-ramp
results there were already flagged `_distrusted_2026-09-12` in `rig-profile.json` pending a
hardware check; the ground loop is very plausibly what that check found, on top of the rig
having physically changed since). Runs from **2026-09-13 onward** are **v3**, direct
guitar-to-Bela, unbuffered, until a passive DI box is added and the profile bumps again.
