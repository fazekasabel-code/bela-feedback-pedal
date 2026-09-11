# Agent instructions — bela-feedback-pedal

These are binding rules for any agent session (Claude, Cursor) working in this repo.
The full reasoning is in `docs/ground-rules-and-facts.md`; this file is the enforceable summary.

## Current platform

**Bela Gem Stereo (Starter Kit, PocketBeagle 2 base).** The rig as actually wired,
2026-09-11: Epiphone (humbucker) → **M4** (buffered instrument input, direct out) → Bela Gem
audio in; DSP on the board (C++, or SuperCollider as in the existing one-cell work); Bela
audio out → **power amp (Dayton DTA120) → exciter (Dayton)**, straight through, no pedal in
between. `docs/phase-plan.md` is the live plan. The physics, DSP architecture and metrics
are platform-independent — see `docs/ground-rules-and-facts.md` §3.1 for the Gem's hardware
facts (24-bit/96 kHz, sub-ms latency, no analog out, 8 analog in retained, new browser IDE),
§3.2 for the rest of the chain, and §4.5 for the 2026-09-11 fail-safe decision this wiring
relies on (no hardware kill switch — see rule 6 below). Almost all of the Gem's own hardware
numbers are unverified on the unit until Phase 0/1 measures them.

## Read first

- `docs/ground-rules-and-facts.md` — facts, constraints, DSP architecture, safety (§3.1 is
  the Gem Stereo hardware, §4.5 is the fail-safe decision for this build's wiring)
- `docs/phase-plan.md` — the live sequence of work
- `rig-profile.json` — the measured truth about the physical rig (v2, Gem Stereo, all null)

Never contradict the ground-rules doc silently. If the work requires changing a fact or a
constraint in it, change the doc in the same commit and say so.

## Safety — hard rules, no exceptions

1. There is a hard output ceiling in the DSP. **No automatic tuning, parameter search, or
   optimisation process may raise it.** It changes only by an explicit human commit.
2. Real-loop tests (feedback actually running through the exciter) require Abel to have
   confirmed the rig is live and to be **present** (ground-rules §10.1). This build has no
   dedicated hardware kill switch to name — see rule 6 for why, and for what presence means
   here instead. Unattended runs are not permitted until Abel explicitly grants that on this
   rig, once it is built and he has confirmed it connected.
3. Real-loop runs are watchdogged: no run continues past its time box. On timeout, mute.
4. Any xrun, NaN, denormal storm or lost connection ⇒ mute the output. Do not attempt
   to recover and keep running.
5. Power amp gain and exciter mounting position are fixed constants of the rig profile.
   Never ask for them to be changed mid-session to make a test pass.
6. **There is no dedicated hardware kill switch in this build.** Bela audio out feeds the
   Dayton DTA120 power amp directly into the exciter — no pedal or other device in between.
   This is a deliberate decision (Abel, 2026-09-11), not an oversight: see ground-rules §4.5
   for the full reasoning — no separate uncontrolled acoustic loop exists in this build (rule
   8), and the controlled loop's worst case through the DTA120 + exciter is judged safe even
   at full runaway ("the signal explodes"). Rules 1, 3 and 4 are the safety net instead, and
   they do not become optional because of this. If a hardware kill (e.g. a volume pedal) is
   ever added to this chain, name it here and it becomes mandatory again.
7. No monitoring mix or hardware passthrough on the Bela may feed the audio input back to
   the output outside the DSP — that closes an analog loop the DSP cannot see or mute.
   Check every session.
8. **No separate acoustic amplification chain exists in this build.** The guitar goes only
   into the M4 → Bela; nothing feeds a live guitar amp or PA, so the uncontrolled acoustic
   loop this rule exists to prevent isn't wired at all right now (ground-rules §2.3). If a
   guitar amp or PA is ever introduced — for playing, or for anything else — it must stay
   dead during development, rule 2 applies in its strict form, and rule 6's reasoning no
   longer holds until a hardware kill is reintroduced.
9. Safety-critical code — the output ceiling, the watchdog, the mute paths — is written
   here, not delegated to Cursor.

## Process

10. **One variable at a time.** Every run is a named parameter set, committed, A/B'd
   against the current best.
11. Every test run writes a log record to `logs/`: git commit hash, rig-profile hash, the
   full parameter set, all proxy metrics, and the path to the recorded audio.
   A result without its parameter set is not a result.
12. **Simulator first, hardware second.** Parameter *searches* run against the offline
   loop simulator (never built yet — see `phase-plan.md` Phase 2). The real rig is for
   validation and musical judgement, not grid search.
13. If the rig physically changes — different platform, different guitar, exciter moved,
   amp gain touched — the rig profile is re-measured and its version bumped, and prior
   results are tagged as belonging to the old profile (see `logs/README.md`). Current:
   `rig-profile.json` is v2, all measurements null.
14. `main` always boots and makes sound. Work on branches.
15. When you are unsure whether a change is a correctness fix or a taste decision, it is a
    taste decision. Stop and ask Abel.
16. The proxy metrics in `host/harness/metrics.py` are a stand-in for Abel's ears. When his
    rating of a take disagrees with the metrics, **the metric is the bug** — rewrite the
    metric, do not argue for the tuning.

## Vocabulary

- **"Suppression"** in this project always means *regulation to a target level*, never removal.
  The actuator is a per-partial gain cell with a level target, not a fixed-depth notch.
- **Controlled loop** = pickup → M4 (buffered direct out) → Bela Gem audio in → DSP → Bela
  Gem audio out → power amp (Dayton DTA120) → exciter (Dayton) → body → strings → pickup.
- **Uncontrolled loop** = amp → air → guitar. Not wired in this build at all (rule 8); kept
  as a concept for if/when a guitar amp is reintroduced (phase-plan Phase 7).
- **Cell** = one adaptive gain-regulation unit bound to one partial.
- **Rig profile** = the versioned file of measured facts about the physical setup.
