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
