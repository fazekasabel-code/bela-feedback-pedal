# Gen1 — Phased Semi-Autonomous Development Plan

Companion to **Gen1 Feedback Regulator — Ground Rules & Facts**. That doc holds the facts and constraints; this one holds the sequence.

**The organising idea:** the agent can only be autonomous where it can *close the loop without a room, a guitar and a pair of ears*. So the plan front-loads two things — instrumentation (the agent can see what the DSP sees) and a loop simulator (the agent can run a thousand experiments overnight). Everything after that is fast. Everything before that is Abel in the room with a multimeter.

Each phase below states: **what gets built**, **who is needed**, and the **exit criterion** that unlocks the next phase.

---

## Phase 0 — Bring-up and the development spine

*Abel present for the physical parts; agent does most of it once it can reach the board.*

- Flash the Bela image, get networking up (USB gadget at `root@192.168.7.2`, and/or Ethernet on the LAN so the Mac need not be tethered), set up SSH keys so the agent can build and run without prompts.
- Verify the Bela script workflow end to end: `build_project.sh` (copy + compile + run, `-b` for background, `--watch`), `run_project.sh`, `stop_running.sh`, `set_startup.sh` for boot-time launch.
- Create the repo: `render.cpp` and DSP sources, host-side Python harness, `rig-profile.json`, docs, test logs. `main` always boots and makes sound.
- Cursor/Claude rules file: the ground rules of §10 as enforced project instructions — safety ceiling untouchable, one variable at a time, every run logged with commit hash.
- Hello-world: stereo passthrough with a hard output limiter and a mute-on-error path. This is the skeleton every later version is grown from, and it is the first thing that ever drives the exciter.

**Exit:** the agent can, unattended, edit a file on the Mac, build it onto the Bela, run it, and read back whether it ran — with no human keystrokes in the loop.

---

## Phase 1 — Instrumentation, then characterisation

*Abel in the room. This is the measurement session and it is the foundation of everything.*

**Instrumentation first** — build it before the measurements, so the measurements are captured automatically:

- Integrate Bela's `Watcher` library and **pybela** (websocket streaming, logging, monitoring and control of variables between the board and Python). This is the agent's eyes: it can stream out internal state (detected peaks, per-cell frequency and gain reduction, input/output RMS) and push in parameter changes without recompiling.
- Host harness: run a test, stream the variables, record the audio, compute the §9 proxy metrics, write one log record.

**Then measure the rig:**

1. Round-trip audio latency at the chosen block size; CPU load headroom.
2. Gain staging: pickup level into Bela (target ~-12 dBFS peak on normal playing), Bela out into the volume pedal, power amp gain — then **fix the power amp gain permanently and write it down**.
3. **Exciter→body→pickup transfer function** by swept sine at low level. This is the loop's gain landscape and it produces the ranked candidate list of §7.3.
4. Partial map for the tuning(s) in use.
5. Feedback threshold: volume-pedal position at which the loop reaches unity, and how far above unity it goes at typical settings.
6. Expression pedal: pot value, taper, TRS convention, heel/toe endpoints as read by the ADC (remember the 3.3 V rail into a 0–4.096 V input reads roughly 0 → 0.806).
7. Baseline recording: **the rig with no regulation at all**, feeding back at several volume-pedal positions. This is the "before" that every later result is compared against, and it is what winner-takes-all looks like in the metrics.

**Exit:** `rig-profile.json` v1 exists, is committed, and the harness can compute all §9 metrics from a recorded take automatically.

---

## Phase 2 — The loop simulator ← *this is the autonomy unlock*

*Agent alone, on the Mac.*

Build an offline model of the controlled loop: measured exciter→body→pickup impulse response, plus the loop delay, plus a saturating nonlinearity standing in for the power amp / exciter limit, closed around the DSP under test. Run it faster than real time.

Validate it against the Phase 1 baseline: **the simulator must reproduce winner-takes-all** — one mode taking over and starving the others, at roughly the frequencies the real rig chose. If it does not, the model is wrong and the plan pauses here. This validation step is not optional; a simulator that does not reproduce the disease cannot be trusted to test the cure.

**Exit:** parameter sets can be swept overnight, scored on the §9 metrics, and ranked, with no hardware powered on.

Everything after this point is: **tune in the simulator → shortlist 3–5 candidates → validate on the real rig with Abel → Abel rates them → correct the metrics.**

---

## Phase 3 — Detection layer, actuator disabled

*Agent-heavy. Safe to run on the real rig because it changes nothing.*

Build the analysis layer of §6.1: STFT on an auxiliary task (long window, short hop, quadratic peak interpolation), per-bin magnitude / growth rate / narrowness / persistence, plus the Goertzel candidate bank seeded from the Phase 1 transfer function. Gain cells present but at unity — the signal passes through untouched.

Stream everything to the host and check it against the recorded audio: does it find the right partials, does it score growth correctly, and — the key test — **when the feedback jumps, how many milliseconds before the growth scorer flags the new partial?** That number is the whole §7 thesis, measured.

**Exit:** detection latency after a jump is under the budget, with no false arming during ordinary playing.

---

## Phase 4 — One cell — the first musical milestone

*Simulator for tuning, Abel in the room for the verdict.*

A single gain cell: peaking-EQ biquad with negative gain, driven by an envelope follower targeting a level, fast attack / slow release, interpolated coefficients so it never clicks.

The success test is specific and it is audible: **hold the dominant partial at its target and a second partial should appear on its own.** That is the physics of §5 working. If the second partial does not appear, either the cell is too shallow (the winner still wins) or too deep (loop gain drops below unity everywhere and the feedback dies) — and the diagnosis is in the metrics, not in guesswork.

Tune attack, release, Q and target on the simulator; validate a shortlist on the rig; Abel rates.

**Exit:** on the real rig, a second partial reliably blooms, and feedback sustains rather than dying.

---

## Phase 5 — Multi-cell, allocator, and the jump case

*Mostly agent, in the simulator; periodic real-rig validation sessions.*

- Cell pool N = 4, then 6–8. Allocator per §6.2: arm on growth score, ±50 cent glide window, release with ramp-back, anti-chatter hold and lockout, steal-least-active when full.
- **Jump handling as an explicit test case**, not a hope: a scripted simulator scenario where the loop's favoured mode is switched abruptly, scored on time-to-regulate and on whether the incumbent's cell releases cleanly.
- Optional here, not before: the adaptive-notch fine tracker under FFT supervision (§6.4), evaluated as an A/B against plain interpolated biquads.
- Watch CPU load as N grows.

**Exit:** 3+ simultaneous partials sustained in the simulator *and* on the rig; jump regulation inside the latency budget; no chatter, no clicks, no runaway.

---

## Phase 6 — Physical control

*Abel present for wiring, agent for the mapping.*

Wire the expression pedal (3.3 V / wiper / GND, series resistor, software smoothing, calibrated endpoints from the rig profile) and map it to per-cell target level. Verify the axis is continuous and monotone, that heel-down is genuine transparent bypass, and that the pedal and the volume pedal stay orthogonal — one governs how much energy is in the loop, the other how it is distributed.

Then the part that only Abel can do: **play it.** Sweep the pedal across the range at several volume-pedal settings, record, and decide whether the range is musically well-distributed or whether the useful zone is squeezed into 10% of the travel. Reshape the curve accordingly.

**Exit:** Abel can get from raw winner-takes-all to dense multi-partial texture with his foot, and the whole travel is useful.

---

## Phase 7 — Hardening and musical iteration

*Abel leads; agent supports.*

Boot-at-startup (`set_startup.sh`), so the pedal is an appliance and not a laptop session. A footswitch bypass if wanted. Robustness: what happens on a dropped cable, a muted guitar, a very quiet passage, a hard strum from silence. Gain staging re-checked end to end. Then extended playing sessions, recorded, rated — and the tuning corrected from the ratings.

Reintroduce **amp volume** as a deliberate variable at the very end (§2.3): the acoustic loop through the amp is uncontrolled and will fight the regulator. Find out how much amp is usable before the pedal loses authority.

**Exit = gen1 done:** plug in, switch on, play. Feedback sustains on 3+ partials, does not collapse to a single tone, tracks jumps, and the expression pedal is musical across its travel.

---

## Division of labour, summarised

| | Agent can do alone | Needs Abel in the room |
|---|---|---|
| Phase 0 | build/run/deploy plumbing, repo, rules | flashing, cabling, first exciter test |
| Phase 1 | harness, metric code, log format | every measurement, gain staging, baseline takes |
| Phase 2 | the whole simulator | validating it feels/measures like the real rig |
| Phase 3 | detector, latency analysis | one real-loop session to log against |
| Phase 4 | parameter search in the simulator | the verdict on whether it sounds like anything |
| Phase 5 | allocator, jump tests, sweeps | periodic validation, ratings |
| Phase 6 | mapping and curve shaping | wiring, and playing it |
| Phase 7 | robustness, boot config | all of the musical judgement |

**The recurring cycle, from Phase 3 onward:** agent tunes against proxy metrics in the simulator → shortlists → Abel validates on the rig and rates the takes → disagreements between rating and metric are treated as bugs *in the metric* → metric rewritten → repeat. That cycle is what makes this semi-autonomous rather than either fully manual or fantasy-autonomous.

---

## Risks worth naming now

| Risk | Mitigation |
|---|---|
| Simulator does not reproduce the real rig's behaviour | Phase 2 validation gate — do not proceed on an unvalidated model |
| Jump detection is too slow in practice | Growth-rate scoring + Goertzel candidate bank (§7) are both designed for exactly this; measured explicitly in Phase 3 |
| Regulation kills the feedback instead of redistributing it | Target-level cells, not fixed notches (§5); "sustain" is a first-class metric |
| Proxy metrics reward something ugly | Ratings are ground truth; metrics are rewritten when they disagree |
| Passive pickup into Bela's ~20 kΩ input degrades detection | Buffered split is a ground rule, not an option |
| Uncontrolled acoustic loop through the amp masks everything | Amp silent during all development; reintroduced only in Phase 7 |
| CPU ceiling on the single-core Cortex-A8 | Load tracked from Phase 3 on; Goertzel bank is cheap by design; N is a tunable |
