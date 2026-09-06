# Agent instructions — bela-feedback-pedal

These are binding rules for any agent session (Claude, Cursor) working in this repo.
The full reasoning is in `docs/ground-rules-and-facts.md`; this file is the enforceable summary.

## Read first

- `docs/ground-rules-and-facts.md` — facts, constraints, DSP architecture, safety
- `docs/phase-plan.md` — what phase we are in and what its exit criterion is
- `rig-profile.json` — the measured truth about the physical rig

Never contradict the ground-rules doc silently. If the work requires changing a fact or a
constraint in it, change the doc in the same commit and say so.

## Safety — hard rules, no exceptions

1. There is a hard output ceiling in the DSP. **No automatic tuning, parameter search, or
   optimisation process may raise it.** It changes only by an explicit human commit.
2. Real-loop tests (feedback actually running through the exciter) require Abel to have
   confirmed the rig is live and that he is present with the volume pedal in reach.
   Never start one on your own initiative.
3. Real-loop runs are watchdogged: no run continues past its time box without a fresh
   heartbeat from the host. On timeout, mute.
4. Any xrun, NaN, denormal storm or lost connection ⇒ mute the output. Do not attempt
   to recover and keep running.
5. Power amp gain and exciter mounting position are fixed constants of the rig profile.
   Never ask for them to be changed mid-session to make a test pass.

## Process

6. **One variable at a time.** Every run is a named parameter set, committed, A/B'd
   against the current best.
7. Every test run writes a log record to `logs/`: git commit hash, rig-profile hash, the
   full parameter set, all proxy metrics, and the path to the recorded audio.
   A result without its parameter set is not a result.
8. **Simulator first, hardware second.** Parameter searches run against the offline loop
   simulator. The real rig is for validation and musical judgement, not grid search.
9. If the rig physically changes — different guitar, exciter moved, amp gain touched —
   the rig profile is re-measured and its version bumped, and prior results are tagged as
   belonging to the old profile.
10. `main` always boots and makes sound. Work on branches.
11. When you are unsure whether a change is a correctness fix or a taste decision, it is a
    taste decision. Stop and ask Abel.
12. The proxy metrics in `host/harness/metrics.py` are a stand-in for Abel's ears. When his
    rating of a take disagrees with the metrics, **the metric is the bug** — rewrite the
    metric, do not argue for the tuning.

## Vocabulary

- **"Suppression"** in this project always means *regulation to a target level*, never removal.
  The actuator is a per-partial gain cell with a level target, not a fixed-depth notch.
- **Controlled loop** = pickup → Bela → volume pedal → power amp → exciter → body → strings → pickup.
- **Uncontrolled loop** = amp → air → guitar. Kept silent during development.
- **Cell** = one adaptive gain-regulation unit bound to one partial.
- **Rig profile** = the versioned file of measured facts about the physical setup.
