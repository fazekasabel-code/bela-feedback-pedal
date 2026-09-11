# Gen1 — Phased Semi-Autonomous Development Plan

Companion to **Gen1 Feedback Regulator — Ground Rules & Facts**. That doc holds the facts and constraints; this one holds the sequence.

**The organising idea:** the agent can only be autonomous where it can *close the loop without a room, a guitar and a pair of ears*. So the plan front-loads two things — instrumentation (the agent can see what the DSP sees) and a loop simulator (the agent can run a thousand experiments overnight). Everything after that is fast. Everything before that is Abel in the room with a multimeter.

Each phase below states: **what gets built**, **who is needed**, and the **exit criterion** that unlocks the next phase.

---

> **Status.** Not starting from zero: the proxy metrics (`host/harness/metrics.py`), the
> log-record writer (`host/harness/logrecord.py`), an I/O bring-up tool, and a working
> **one adaptive cell** (`sc/gen1_cell.scd`) already exist and have **passed the "second
> partial blooms" test** (Phase 4). That run exposed a detector defect — pitch tracking
> finds a fundamental, not the dominant partial — and replacing it with the FFT peak
> picking of §6.1 is the first real DSP task here (Phase 3). The Phase 2 loop simulator has
> **not been built yet**. Every Gem hardware number this plan assumes is unverified until
> Phase 0/1 measures it — see **§3.1** in
> [`ground-rules-and-facts.md`](ground-rules-and-facts.md).

---

## Phase 0 — Bring-up and the development spine

*Abel present for the physical parts; agent does most of it once it can reach the board.*

- **Done, 2026-09-11.** Board reachable at `root@192.168.7.2` over the USB-C gadget address; key-based SSH works with no prompts. The browser IDE hasn't been tried, but the CLI path the agent actually needs is live.
- Verify the Bela script workflow end to end: `build_project.sh` (copy + compile + run, `-b` for background, `--watch`), `run_project.sh`, `stop_running.sh`, `set_startup.sh` for boot-time launch. **Partly done, 2026-09-11:** `build_project.sh` (foreground and `-b`/`--force`) and `stop_running.sh` confirmed working, run directly on the board (`Bela/scripts/`) against a project `scp`'d there — see `bela/io-check-input/` below. `scripts/deploy.sh`'s own host-side path (which assumes a local Bela repo clone on the Mac) is still unverified; `run_project.sh` / `set_startup.sh` untried.
- Create the repo: `render.cpp` and DSP sources, host-side Python harness, `rig-profile.json`, docs, test logs. `main` always boots and makes sound.
- Cursor/Claude rules file: the ground rules of §10 as enforced project instructions — safety ceiling untouchable, one variable at a time, every run logged with commit hash.
- Hello-world: stereo passthrough with a hard output limiter and a mute-on-error path (`bela/gen1-passthrough/`). **Built and run on hardware, 2026-09-11** — a watchdog (hard 120 s time-box, latched mute, matching `rig-profile.json`'s `safety.watchdog_timeout_s`) was added before this ever touched the exciter, since ground rule 3 has no exception for "no host heartbeat channel yet." Ran clean for ~13 s with Abel present and the rig live end to end (Epiphone → M4 → Bela → DTA120 → exciter → body → pickup): no mute, no error, watchdog armed but not tripped. Stopped deliberately rather than left running — first real-loop smoke test, not a listening session yet.
- **Input signal confirmed, 2026-09-11.** `bela/io-check-input/` — a throwaway, output-silent diagnostic (never calls `audioWrite`, so it cannot drive the exciter regardless of how the outputs are patched) — was built, deployed and run to check the real signal chain: Epiphone (humbucker) → M4 (buffered direct out) → Bela Gem audio in. First pass: both channels pinned at 0 dBFS peak, clipping. Abel turned the source down; re-check came back clean at ch0 ≈ ‑19 dBFS peak / ‑31 dBFS RMS, ch1 ≈ ‑19 dBFS peak / ‑32 dBFS RMS, matched L/R, no clipping. Board reported 2 in / 2 out @ 44100 Hz, block size 16 at default project settings — not yet the deliberate Phase 1 choice. Logged in `rig-profile.json` → `host.input_signal_check`.

**Exit:** the agent can, unattended, edit a file on the Mac, build it onto the Bela, run it, and read back whether it ran — with no human keystrokes in the loop. **Mostly there:** build/run/stop confirmed working headlessly over SSH, an input-signal check and a first real-loop passthrough smoke test both passed clean. What's left: a longer/louder passthrough listening session with Abel judging whether the loop actually feeds back at the DTA120's current gain (that's really Phase 1 gain-staging territory), and closing out `scripts/deploy.sh` / `run_project.sh` / `set_startup.sh`.

---

## Phase 1 — Instrumentation, then characterisation

*Abel in the room. This is the measurement session and it is the foundation of everything.*

**Instrumentation first** — build it before the measurements, so the measurements are captured automatically:

- Integrate Bela's `Watcher` library and **pybela** (websocket streaming, logging, monitoring and control of variables between the board and Python). This is the agent's eyes: it can stream out internal state (detected peaks, per-cell frequency and gain reduction, input/output RMS) and push in parameter changes without recompiling. **Confirmed working end to end, 2026-09-11** (ground-rules §3.1, open question 3, resolved with caveats): `bela/watcher-check/` streams `audio_in_ch0`/`audio_in_ch1` at audio rate; `host/rig/watcher_check.py` connected, streamed 2000 values per channel, and read back real (near-silent, no signal playing) input levels. Getting there needed vendoring `Watcher.h`/`.cpp` (not core Bela), two small compile fixes for this board's toolchain, a compat shim for a stale PyPI `pybela` release, and Python ≤3.12 on the host — see ground-rules §3.1 for the details, they'll matter for every project that uses Watcher from here on.
- Host harness: run a test, stream the variables, record the audio, compute the §9 proxy metrics, write one log record. `host/rig/watcher_check.py` is the first sliver of this — streaming confirmed, but it doesn't yet record audio, compute metrics, or write a log record itself.

**Then measure the rig:**

1. Round-trip audio latency at the chosen block size; CPU load headroom.
2. Gain staging: pickup level into Bela (target ~-12 dBFS peak on normal playing), Bela out straight into the power amp (Dayton DTA120) — no pedal in between in this build, see ground-rules §4.5 — then **fix the power amp gain permanently and write it down**.
3. **Exciter→body→pickup transfer function** by swept sine at low level. This is the loop's gain landscape and it produces the ranked candidate list of §7.3.
4. Partial map for the tuning(s) in use.
5. Feedback threshold: volume-pedal position at which the loop reaches unity, and how far above unity it goes at typical settings.
6. Expression pedal: pot value, taper, TRS convention, heel/toe endpoints as read by the ADC — calibrate and store these, never assume a range.
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

**Growth-rate arm/release detector built and running, 2026-09-12 — the Goertzel bank is not.** The bank needs the Phase 1 exciter→body→pickup transfer function (§7.3) as its candidate list, and that measurement hasn't happened (gain staging is still provisional, per Abel — exact numbers wait for the final guitar/buffer). Built instead:

- `host/harness/detector.py` — the reference: a streaming, frame-causal per-bin growth-rate arm/release detector (window 2048, hop 256, one-pole-smoothed growth, true prominence via `scipy.find_peaks`, persistence, anti-chatter hold). Frame-causal on purpose — it only ever sees the past, exactly like the real-time port has to, so its latency numbers mean something. `host/tests/test_detector.py`: on a synthetic mode growing at 500 dB/s (§7's ballpark for a loop gain near unity), it arms in <50 ms while the new mode is still >6 dB below the incumbent — detection by growth, not by rank, demonstrated on a case with a known right answer. No false arms on a stationary two-tone signal or on quiet noise.
- `bela/detector-passthrough/render.cpp` — the real-time port. Same STFT on a Bela `AuxiliaryTask` (never touches the audio deadline) with the same window/hop/growth logic, same safety net and recording as `harness-passthrough`. One necessary divergence: peak/prominence is a cheap local-shoulder comparison, not `find_peaks`' true prominence walk (real-time-safe, allocation-free) — and it needed a materially different threshold as a result. **Found and fixed the same day:** at the Python module's prominence value (6 dB), the cheap proxy false-armed 150–230 of 1025 bins continuously on a real, quiet (−31 dBFS RMS) room-noise recording (`logs/2026-09-11/outputs.wav`) — not a rare edge case, the steady state of the whole take. Diagnosed and fixed offline in Python against that same recording (sweeping the threshold up to 25 dB reproduced zero false arms, with no change to the synthetic jump-detection latency) before touching hardware again. Redeployed: `armed_count` confirmed 0 throughout a live run via pybela streaming. `logs/2026-09-12/001319_detector-passthrough-check.json` is the hardware validation record.

**What Phase 3's actual exit criterion still needs, honestly:** a real jump, on a real rig, to measure milliseconds-to-flag against. It doesn't exist yet in any recording — the 2026-09-06 Mac-rig "second partial blooms" take (`logs/2026-09-06/gen1_cell_t24.wav`) was checked and is the *wrong kind* of event for this: D4's rise there is a slow, cell-release-mediated bloom (order 5–8 dB/s, over ~15 seconds), not the fast unity-crossing jump §7 describes (order 100–1000 dB/s) — the growth detector correctly does not arm on it, for the same reason it correctly ignores ordinary playing dynamics. A genuine fast jump is something only a real playing session on the live rig will produce; capturing and measuring one against this detector is the real open item here, not a coding task.

**Exit:** detection latency after a jump is under the budget, with no false arming during ordinary playing. **Partly met:** no false arming is now demonstrated both synthetically and against real recorded audio; the latency-after-a-jump half needs a real jump, still to come.

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

Wire the expression pedal (3.3 V / wiper / GND, series resistor, software smoothing, calibrated endpoints from the rig profile) and map it to per-cell target level. Verify the axis is continuous and monotone, that heel-down is genuine transparent bypass, and that the pedal stays orthogonal to loop gain. Loop gain is presently fixed by the DTA120's own gain control, not a pedal (§4.5); if a volume pedal is reintroduced later for foot control of loop gain, the same orthogonality check applies — one governs how much energy is in the loop, the other how it is distributed.

The analog-in full-scale voltage is unconfirmed (ground-rules §3.1, open question 2) — do not assume a range; calibrate from the endpoints stored in the rig profile. MIDI over the USB-A host is available as a fallback control path if the analog pedal input turns out not to work as expected.

Then the part that only Abel can do: **play it.** Sweep the pedal across the range at several loop-gain settings, record, and decide whether the range is musically well-distributed or whether the useful zone is squeezed into 10% of the travel. Reshape the curve accordingly.

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
| Passive pickup into an unbuffered board input degrades detection | The M4's buffered instrument input is a ground rule, not an option |
| Uncontrolled acoustic loop through the amp masks everything | No amp is wired into this build at all during development; reintroduced only in Phase 7, at which point ground-rules §4.5's "no hardware kill needed" reasoning must be revisited |
| CPU ceiling on PocketBeagle 2 | Load tracked from Phase 3 on; Goertzel bank is cheap by design; N is a tunable |
