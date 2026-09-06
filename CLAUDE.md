# Agent instructions — bela-feedback-pedal

These are binding rules for any agent session (Claude, Cursor) working in this repo.
The full reasoning is in `docs/ground-rules-and-facts.md`; this file is the enforceable summary.

## Current platform

**The Bela is not connectable. Gen1 is being built on a Mac + Focusrite Scarlett 8i6.**
Guitar into input 1 (INST), exciter chain off outputs 3/4, DSP in Python in the audio
callback. `docs/mac-rig-plan.md` is the live plan; `docs/phase-plan.md` is the Bela plan,
parked. The physics, DSP architecture and metrics are unchanged by the move.

## Read first

- `docs/ground-rules-and-facts.md` — facts, constraints, DSP architecture, safety
  (see the 2026-09-06 amendment, and §3.3 / §4.6 for the Mac rig)
- `docs/mac-rig-plan.md` — the current plan and which phase we are in
- `rig-profile.json` — the measured truth about the physical rig

Never contradict the ground-rules doc silently. If the work requires changing a fact or a
constraint in it, change the doc in the same commit and say so.

## Safety — hard rules, no exceptions

1. There is a hard output ceiling in the DSP. **No automatic tuning, parameter search, or
   optimisation process may raise it.** It changes only by an explicit human commit.
2. Real-loop tests (feedback actually running through the exciter) require Abel to have
   confirmed the rig is live and that he is present with **a hardware attenuator in reach**.
   Never start one on your own initiative. On the Mac rig that attenuator is the headphone-2
   level knob or the power amp's own control — there is no volume pedal (Abel, 2026-09-06).
3. Real-loop runs are watchdogged: no run continues past its time box. On timeout, mute.
   Abel has accepted sustained unchecked feedback on this rig (2026-09-06), because the
   guitar-amp path is dead and only the exciter loop is live, so the time box is generous
   rather than tight — but it exists, so a runaway can never outlive the session that
   started it.
4. Any xrun, NaN, denormal storm or lost connection ⇒ mute the output. Do not attempt
   to recover and keep running.
5. Power amp gain and exciter mounting position are fixed constants of the rig profile.
   Never ask for them to be changed mid-session to make a test pass.
6. **On the Mac rig, the Scarlett's front monitor knob is not the kill switch.** Outputs
   3/4 leave via the **second headphone jack**, which has its own level knob — that knob,
   or the power amp's own control, is the designated kill. There is no volume pedal.
   Name the kill out loud before any live-loop run.
7. **Focusrite Control must feed outputs 3/4 from Playback 3/4 only.** A hardware mix
   carrying Input 1 to those outputs closes an analog loop the DSP cannot see or mute.
   Confirmed correct by Abel, 2026-09-06. Re-check every session; it is one click away.
8. **The guitar amp stays dead.** The whole basis for allowing unchecked sustained feedback
   here is that only the controlled loop is live. If the guitar amp is ever switched on,
   that permission lapses and rule 2 applies in its strict form again.
9. Safety-critical code — the output ceiling, the watchdog, the mute paths — is written
   here, not delegated to Cursor.

## Process

10. **One variable at a time.** Every run is a named parameter set, committed, A/B'd
   against the current best.
11. Every test run writes a log record to `logs/`: git commit hash, rig-profile hash, the
   full parameter set, all proxy metrics, and the path to the recorded audio.
   A result without its parameter set is not a result.
12. **Simulator first, hardware second.** Parameter *searches* run against the offline
   loop simulator. The real rig is for validation and musical judgement, not grid search.
   (Proposed narrowing on the Mac rig, pending Abel's sign-off: a single-hypothesis A/B may
   go straight to the rig, now that the deploy step that made rig time expensive is gone.
   Grid search still goes to the simulator. Until he signs off, the rule stands as written.)
13. If the rig physically changes — different guitar, exciter moved, amp gain touched —
   the rig profile is re-measured and its version bumped, and prior results are tagged as
   belonging to the old profile.
14. `main` always boots and makes sound. Work on branches.
15. When you are unsure whether a change is a correctness fix or a taste decision, it is a
    taste decision. Stop and ask Abel.
16. The proxy metrics in `host/harness/metrics.py` are a stand-in for Abel's ears. When his
    rating of a take disagrees with the metrics, **the metric is the bug** — rewrite the
    metric, do not argue for the tuning.

## Vocabulary

- **"Suppression"** in this project always means *regulation to a target level*, never removal.
  The actuator is a per-partial gain cell with a level target, not a fixed-depth notch.
- **Controlled loop** = pickup → Bela → volume pedal → power amp → exciter → body → strings → pickup.
- **Controlled loop (Mac rig)** = pickup → Scarlett in 1 → Mac DSP → Scarlett out 3/4 →
  attenuator → power amp → exciter → body → strings → pickup.
- **Uncontrolled loop** = amp → air → guitar. Kept silent during development.
- **Cell** = one adaptive gain-regulation unit bound to one partial.
- **Rig profile** = the versioned file of measured facts about the physical setup.
