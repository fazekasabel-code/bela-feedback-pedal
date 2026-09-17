# Gen1 — Phased Semi-Autonomous Development Plan

Companion to **Gen1 Feedback Regulator — Ground Rules & Facts**. That doc holds the facts and constraints; this one holds the sequence.

**The organising idea:** the agent can only be autonomous where it can *close the loop without a room, a guitar and a pair of ears*. So the plan front-loads two things — instrumentation (the agent can see what the DSP sees) and a loop simulator (the agent can run a thousand experiments overnight). Everything after that is fast. Everything before that is Abel in the room with a multimeter.

Each phase below states: **what gets built**, **who is needed**, and the **exit criterion** that unlocks the next phase.

---

> **Status, 2026-09-13 — Phase 4 passed, Phase 5 built and awaiting its real-loop session.** (Superseded by the 2026-09-17 block below — Phase 5 has now had its sessions.) Rig
> is **v4** in `rig-profile.json` (bumped from v3 same day, see below). The M4
> buffer/preamp is **out** (confirmed ground-loop source, see ground-rules §4.4), guitar runs
> **direct/unbuffered into Bela ch0**, exciter is fed from Bela **ch1** via the DTA120 — both
> confirmed by direct measurement, not assumption. Measured this session: input gain staged
> to **10 dB** on Bela's own PGA (ch0 lands ‑8.8 to ‑11.6 dBFS peak, mezzoforte); whole-loop
> round-trip latency **2.20 ms** (`bela/latency-check/`, through the real exciter→body→pickup
> path, not a cable loopback); feedback threshold bracketed between DTA120=12 o'clock (no
> growth, even at loop-gain 1.0 held 60s) and 100% (threshold ≈ loop-gain 1.0, gentle ~1 dB/s
> growth) — not narrowed further, Abel's call. The growth-rate detector (Phase 3,
> `bela/detector-passthrough/`) exists and runs, but **its arm threshold is now known
> miscalibrated**: it reliably catches large/fast manufactured jumps (gain≥2, arms in
> 45–84 ms) but misses the realistic near-unity case (gain 1.0–1.5, clean single partial,
> confirmed by `analyse_take.py`) that actually matters — flagged, not fixed, deliberately
> deferred past Phase 4. **Phase 2 (the loop simulator) is deliberately skipped for now**,
> not forgotten: it needs a transfer-function measurement that hasn't been done, and its
> whole value (autonomous overnight search without Abel present) matters less while Abel is
> available for iterative real-rig testing — revisit once Phase 5's wider parameter space
> makes that need concrete. **Phase 4 PASSED, 2026-09-13.** `bela/gen1-cell/` ports
> `sc/gen1_cell.scd`'s one adaptive cell to C++, compiled clean on this board's toolchain and
> then run for real: DTA120=100%, Abel present, 30 s, no mute/watchdog/xrun
> (`logs/2026-09-13/185836_first-cell-take-dta120-100pct.json`). One deliberate divergence
> from the SC patch, worth flagging because it isn't just a port: rather than SC's
> autocorrelation pitch tracker, the cell binds to a partial via the Phase 3 STFT
> growth-detector's machinery (already hardware-validated), but **by magnitude/rank, not by
> growth rate** — the growth-arm threshold logged above as miscalibrated for this rig's real
> (~1-2.5 dB/s) growth would otherwise likely never fire for the case this cell exists to
> regulate. See `bela/gen1-cell/render.cpp`'s header comment for the full reasoning, including
> the other deliberate simplification (one envelope-follower stage, not SC's two cascaded
> ones). **Abel's verdict on the take, listening: pass** — "a nicely blooming chord, always
> 3-4 notes present," more than the minimum second-partial the exit test asks for. Some
> distortion audible from ~16s on, cause undetermined between two live candidates: the DSP
> output ceiling (which the take did hit) or the exciter itself, which turns out to **not be
> fixed/mounted** — a genuinely new rig fact, not a mid-session change, now in `rig-profile.json`
> `exciter._mounting_note_2026-09-13`. Abel's call: proceed to Phase 5, revisit the distortion
> source later. **Same day, before Phase 5 work started: the exciter was secured better**
> (Abel) — CLAUDE.md rule 13 ("exciter moved") applies, `rig-profile.json` is bumped to **v4**,
> and every v3 real-loop measurement/take above (round-trip latency, the feedback-threshold
> bracket, the Phase 4 pass itself) is tagged as measured through the old, looser mount — see
> `logs/README.md` for the v3/v4 boundary (same day, same directory — go by
> `provenance.rig_profile_sha256` or the filename, not the date). **Phase 5 is built and
> partly validated.** `bela/gen1-multicell/` generalises gen1-cell to N=4 cells with a proper
> allocator (glide, release-with-ramp-back, anti-chatter, lockout, steal-least-active) and
> Bela's own CPU-load monitoring wired to Watcher. Two live-playing takes read closer to
> single-partial than gen1-cell's pass — investigated three ways (all logged, see phase-plan
> Phase 5 section below for the full writeup): a new time-windowed re-scoring tool showed the
> 40s take actually was trending multi-partial, just slowly; a new engaged/disengaged A/B
> (a sweep-kick seeds the loop with no human needed, a `bypass` flag toggles the cells) showed
> a clean, striking result — engaged locks onto a stable 4-partial texture for 30+s, disengaged
> decays back to winner-takes-all; and a new offline sandbox validated the actuator/allocator
> logic against synthetic tones and surfaced one real, unfixed finding — a bin-exclusion radius
> that may be too wide for closely-spaced guitar partials, worth revisiting once the (still
> unmeasured) partial map exists. **Same day, three more of Abel's requests, one pass:** cell
> pool bumped **N=4 → 12** (past the plan's own "4, then 6-8" — CPU headroom checked live, not
> assumed: ~32% audio-thread CPU at N=12, comfortable; a units bug in the CPU%
> reporting itself was caught and fixed in the process); **steal made click-safe** — a "rebind
> duck" ramps the applied cut to 0 and back over 50ms on every fresh bind or steal (the
> frequency slew alone wasn't enough — it let an old, unrelated cut sweep smoothly THROUGH the
> new target frequency instead of jumping there, still audible as a notch); and
> **`bela/gen1-multicell-live/`** — the same N=12/duck-safe allocator with the sweep-kick
> removed entirely (for live playing). The browser GUI looked genuinely, reproducibly broken
> through a long investigation (three theories chased and ruled out with real evidence: `dev`
> branch, Chrome's Local Network Access policy, a non-standard HTTP reason phrase — see git
> history if any of that matters later) — **and then it just worked in Abel's actual Edge
> browser.** Never fully explained; the working theory is that the retry loop needs more
> patience than this session's own quick automated checks ever gave it, not a real fix. Along
> the way: Watcher's upstream repo ships a `sketch.js` (p5.js frontend) never vendored here
> before — now is, and replaced with a **custom view** (12-cell grid, summary row) instead of
> the generic 53-row variable table. **Also added: five live tuning sliders** (target level,
> max cut, release time, cell Q, a new software loop-gain multiplier) **plus a bypass
> checkbox**, all Watcher-controllable from the browser, all clamped in code regardless of
> what's sent, verified end to end with a raw websocket test (not assumed). Found a second
> real bug while wiring these up: a Watcher variable needs an explicit `.localControl(false)`
> call to actually be settable remotely — `bypass` had the same latent gap, fixed here and in
> `gen1-multicell`. The pybela per-sample-write bug from earlier is unaffected, still fixed,
> still flagged as a follow-up for `detector-passthrough`/`gen1-cell`/`feedback-ramp`/
> `harness-passthrough` (now bundled with the `localControl` fix in the same follow-up task).
> `host/rig/multicell_monitor.py` remains available as a terminal fallback if the browser view
> ever flakes again. **Not yet done:** an actual live-playing/tuning session — the tooling is
> now confirmed working twice over (pybela AND the browser), nobody's played through it yet.
> See phase-plan Phase 5 section for what's
> not done otherwise (growth-rate arming, the formal jump test, the optional adaptive notch).
> Everything above is logged
> under `logs/2026-09-13/`; see `docs/ground-rules-and-facts.md` §4.4 for the ground-loop
> story and phase-plan Phase 1/3 sections below for the full measurement writeups.

> **Status, 2026-09-17 — Phase 5's first real tuning sessions. Abel's verdict: "behaves fairly
> well."** The session started from his report that the texture was still not a cluster:
> "usually the low E string vibrating extremely and the rest are quiet." Five fixes, in the
> order they were found, each one surfaced by playing rather than by analysis:
>
> 1. **The detector could not see the low E.** The 2026-09-14 change claimed a 43 Hz detection
>    floor; it shipped 86.1 Hz. The window change alone would have given 43 Hz, but the
>    band-limiting optimisation landed in the same commit rewrote the prominence guard to be
>    relative to the analysed band, so the band's 45 Hz floor was *added* to the 8-bin shoulder
>    margin. The low E fundamental (82.41 Hz) is the only standard-tuning string fundamental
>    below that line — it alone could never bind a cell, was never regulated, and ran away into
>    the output ceiling while every other string was held at the target. **This was the whole
>    reported symptom.** The detect band and the filled band are now separate things, the
>    detect band equals the actuator's own frequency range, and `setup()` prints the resulting
>    floor in Hz and warns if it lands above the low E — a comment asserting a number the code
>    did not produce is how this survived. Floor is now 53.8 Hz at 44.1 kHz. **Open:** the
>    shoulder margin costs 8 *bins*, so at 96 kHz with this window the floor returns to 105 Hz;
>    reachable by changing the sample rate alone, which is still an open Phase 0/1 choice.
> 2. **Two cells doubling on one partial** (seen live straight after fix 1: cells 0 and 11 both
>    on 80 Hz, cells 6 and 9 both on 734 Hz). The keep-out radius was enforced in exactly one
>    place, at candidate-gather time, which covers binding and nothing else — the glide had no
>    occupancy check at all, so a cell that bound to a sidelobe walked uphill into the peak and
>    collided. Not merely a wasted cell: every cell detects from the raw pre-cell input, so two
>    cells on one partial each compute the *full* correction and both apply it, doubling the
>    control loop's gain and driving that partial well below target. Now enforced at all three
>    points where it can break, with a collision-resolution pass in which the newcomer gives way.
> 3. **An audible click on rebind under load.** The rebind duck ramped *in* over 50 ms but went
>    *out* via a single-sample assignment to zero — a gain step whose size is the cut the cell
>    was holding. Measured: 15.000 dB/sample before, 0.023 dB/sample after. The frequency is now
>    also held until the cut is fully out, which is what ducking was for in the first place.
> 4. **Envelope attack reclassified and retuned.** Raised by Abel against plucked notes: at 3 ms
>    the envelope tracks the pluck transient (20–30 dB above the sustain level) and flattens the
>    attack of every note. See ground-rules §6.3's 2026-09-17 revision for the argument — the
>    rule ("faster than loop growth") is right, the 1–5 ms value was ~300× tighter than it needs
>    to be, and a slow attack is the correct *discriminator* between fast plucks and slow
>    feedback growth. Now a live control, 1–500 ms, **default 300 ms, set by Abel's ear** (rule
>    16). The safety margin is the bound, not the value.
> 5. **Keep-out radius 110 → 50 cents**, Abel's call, making it exactly the glide window as
>    ground-rules §6.2/§7 already define that boundary. 110 contradicted its own comment (it
>    claimed to let adjacent semitones each hold a cell; a semitone is 100 cents, so that was the
>    one case it blocked) and left a 50–110 cent band that could neither be glided onto nor
>    allocated a cell — a partial landing there beside a live incumbent was unregulated for as
>    long as that incumbent held. **This closes the 2026-09-13 sandbox finding flagged above** as
>    "a bin-exclusion radius that may be too wide for closely-spaced guitar partials." Dead zone
>    is gone above ~600 Hz; one bin of it remains below, deliberately, because an 8192-point Hann
>    mainlobe is ~4 bins wide.
>
> Also fixed in passing: the "steal the least active cell" selection used a strict minimum over
> the current cut, so with several cells at exactly 0.0 dB (a normal state — a cell bound below
> the target cuts nothing and is *armed*, not idle) the winner was whichever had the lowest
> index. Now ties break by quietest partial, which is both the least valuable slot and the
> lowest bar for the challenger.
>
> **Not done, deliberately.** `min_hold_ms` is still at its 232 ms default, so the ~280 ms
> release/rebind cycling documented at `kMinBoundHoldFrames` is unchanged — the offline sweep
> suggests 700 ms but ground-rules §6.3 calls release timing musical, so it is Abel's call and
> he has not made it. `prominence_db` was tried at 16 (from 10): "maybe better but no great
> difference," which is itself informative — sidelobe binding is not the dominant churn source.
> Growth-rate arming, the formal jump test and the loop simulator all remain where Phase 5 left
> them. **Gap against CLAUDE.md rule 11:** the live takes this session were Abel playing and
> judging by ear; no log records were written for them, so the parameter sets that produced
> "behaves fairly well" are recorded only as committed constants, not as `logs/` entries.

---

## Phase 0 — Bring-up and the development spine

*Abel present for the physical parts; agent does most of it once it can reach the board.*

- **Done, 2026-09-11.** Board reachable at `root@192.168.7.2` over the USB-C gadget address; key-based SSH works with no prompts. The browser IDE hasn't been tried, but the CLI path the agent actually needs is live.
- Verify the Bela script workflow end to end: `build_project.sh` (copy + compile + run, `-b` for background, `--watch`), `run_project.sh`, `stop_running.sh`, `set_startup.sh` for boot-time launch. **Partly done, 2026-09-11:** `build_project.sh` (foreground and `-b`/`--force`) and `stop_running.sh` confirmed working, run directly on the board (`Bela/scripts/`) against a project `scp`'d there — see `bela/io-check-input/` below. `scripts/deploy.sh`'s own host-side path (which assumes a local Bela repo clone on the Mac) is still unverified; `run_project.sh` / `set_startup.sh` untried.
- Create the repo: `render.cpp` and DSP sources, host-side Python harness, `rig-profile.json`, docs, test logs. `main` always boots and makes sound.
- Cursor/Claude rules file: the ground rules of §10 as enforced project instructions — safety ceiling untouchable, one variable at a time, every run logged with commit hash.
- Hello-world: stereo passthrough with a hard output limiter and a mute-on-error path (`bela/gen1-passthrough/`). **Built and run on hardware, 2026-09-11** — a watchdog (hard 120 s time-box, latched mute, matching `rig-profile.json`'s `safety.watchdog_timeout_s`) was added before this ever touched the exciter, since ground rule 3 has no exception for "no host heartbeat channel yet." Ran clean for ~13 s with Abel present and the rig live end to end (Epiphone → M4 → Bela → DTA120 → exciter → body → pickup): no mute, no error, watchdog armed but not tripped. Stopped deliberately rather than left running — first real-loop smoke test, not a listening session yet.
- **Input signal confirmed, 2026-09-11.** `bela/io-check-input/` — a throwaway, output-silent diagnostic (never calls `audioWrite`, so it cannot drive the exciter regardless of how the outputs are patched) — was built, deployed and run to check the real signal chain: Epiphone (humbucker) → M4 (buffered direct out) → Bela Gem audio in. First pass: both channels pinned at 0 dBFS peak, clipping. Abel turned the source down; re-check came back clean at ch0 ≈ ‑19 dBFS peak / ‑31 dBFS RMS, ch1 ≈ ‑19 dBFS peak / ‑32 dBFS RMS, matched L/R, no clipping. Board reported 2 in / 2 out @ 44100 Hz, block size 16 at default project settings — not yet the deliberate Phase 1 choice. Logged in `rig-profile.json` → `host.input_signal_check`. `bela/io-check-input/` is the one throwaway diagnostic from this period kept in the repo — it's the tool to reach for first any time input sanity needs a quick check.
- **Ground loop found and fixed, 2026-09-12/13.** A full noise investigation (bisected via loopback tests, a Watcher-free control, and a real-signal A/B against the M4) traced loud audio noise to the M4 and Bela sharing ground through the same Mac's USB while also being linked by an analog cable — full story and the fix in ground-rules §4.4. Net effect on this plan: **the M4 is out of the signal path** as of 2026-09-13; the guitar goes directly, unbuffered, into Bela's audio in until a replacement passive DI box arrives. Every throwaway diagnostic built purely for that investigation (`io-check-output`, `io-check-record`, `io-monitor`, `noise-diag`, `noise-diag-watcher`, plus their logged recordings) was deleted once the cause was confirmed — the finding lives in the docs, not in scratch code.

**Exit:** the agent can, unattended, edit a file on the Mac, build it onto the Bela, run it, and read back whether it ran — with no human keystrokes in the loop. **Mostly there:** build/run/stop confirmed working headlessly over SSH, an input-signal check and a first real-loop passthrough smoke test both passed clean. What's left: a longer/louder passthrough listening session with Abel judging whether the loop actually feeds back at the DTA120's current gain (that's really Phase 1 gain-staging territory), and closing out `scripts/deploy.sh` / `run_project.sh` / `set_startup.sh`.

---

## Phase 1 — Instrumentation, then characterisation

*Abel in the room. This is the measurement session and it is the foundation of everything.*

**Instrumentation first** — build it before the measurements, so the measurements are captured automatically:

- Integrate Bela's `Watcher` library and **pybela** (websocket streaming, logging, monitoring and control of variables between the board and Python). This is the agent's eyes: it can stream out internal state (detected peaks, per-cell frequency and gain reduction, input/output RMS) and push in parameter changes without recompiling. **Confirmed working end to end, 2026-09-11** (ground-rules §3.1, open question 3, resolved with caveats): `bela/watcher-check/` streams `audio_in_ch0`/`audio_in_ch1` at audio rate; `host/rig/watcher_check.py` connected, streamed 2000 values per channel, and read back real (near-silent, no signal playing) input levels. Getting there needed vendoring `Watcher.h`/`.cpp` (not core Bela), two small compile fixes for this board's toolchain, a compat shim for a stale PyPI `pybela` release, and Python ≤3.12 on the host — see ground-rules §3.1 for the details, they'll matter for every project that uses Watcher from here on.
- Host harness: run a test, stream the variables, record the audio, compute the §9 proxy metrics, write one log record. **Built, 2026-09-11** — `bela/harness-passthrough/` (records) + `host/rig/harness_run.py` (deploy, run, fetch, score, log) produced a real end-to-end record (`logs/2026-09-11/233408_first-take.json`).

**Then measure the rig:**

1. Round-trip audio latency at the chosen block size; CPU load headroom. **Whole-loop round-trip measured, 2026-09-13** — Abel's suggestion: a short, quiet burst emitted through the exciter (`bela/latency-check/`), found on the pickup by cross-correlation (`host/rig/analyse_latency.py`), same method as the old Mac-rig `io_check.py` cable-loopback test but through the real mechanical/acoustic path this time, not a direct cable. Median 2.20 ms (spread 2.02 ms across 5 bursts, DTA120 at 100%) — see `rig-profile.json` `host.round_trip_latency_ms`. CPU load headroom still unmeasured.
2. Gain staging: pickup level into Bela (target ~-12 dBFS peak on normal playing), Bela out straight into the power amp (Dayton DTA120) — no pedal in between in this build, see ground-rules §4.5 — then **fix the power amp gain permanently and write it down**. **M4 preamp staged live, 2026-09-12, retired 2026-09-13** — the M4 was removed from the signal path (confirmed source of a ground loop, see ground-rules §4.4), so that measurement belongs to a rig that no longer exists in this form. **Redone from scratch, 2026-09-13, without the DI box** — with the M4 gone the only gain stage left is Bela's own input PGA (0–59.5 dB). Staged live against `bela/io-check-input/` while Abel played mezzoforte: 16 dB (board default) showed nothing above the noise floor, 50 dB and 25 dB both clipped hard, **10 dB landed cleanly on target** — ch0 (left) -8.8 to -11.6 dBFS peak / -19.7 to -23.2 dBFS RMS, ch1 silent, no crosstalk. This also confirmed the guitar-input channel index (ch0/left) by direct measurement for the first time — see `rig-profile.json` `host.guitar_input_index` and `gain_staging`. Treat this as provisional: it'll need re-staging once the passive DI box replaces the direct connection (buffering changes the level the board sees). Power amp (DTA120) gain is not yet fixed — see item 5, and its actual output channel (right, per Abel, feeding the exciter) is stated but not yet cross-checked by the tone-sweep method — see `rig-profile.json` `host.exciter_output_index`.
3. **Exciter→body→pickup transfer function** by swept sine at low level. This is the loop's gain landscape and it produces the ranked candidate list of §7.3.
4. Partial map for the tuning(s) in use.
5. Feedback threshold: DTA120 position at which the loop reaches unity, and how far above unity it goes at typical settings (no volume pedal in the loop, see ground-rules §4.5/§8 — DTA120 is presently the loop-gain control). **2026-09-12 attempt DISTRUSTED and retired** — the ground loop later found and fixed (ground-rules §4.4) is a very plausible confound for that session's "zero growth even at 100%" result; see `rig-profile.json` `loop._distrusted_2026-09-12` and `_distrusted_2026-09-12_update_2026-09-13`. **Redone clean, 2026-09-13**, ground-loop-free and with the mono-input/stereo-output channel-routing bug fixed (see item 2's note and `bela/gen1-passthrough`/`bela/feedback-ramp` 2026-09-13 header comments): at DTA120=12 o'clock, zero growth through a full 150s hold at loop-gain=1.0 — corroborated, not contradicted, by Abel directly hearing/feeling only increased sustain (no runaway) live on `gen1-passthrough` at the same setting moments earlier. At DTA120=100% (Abel's deliberate choice to bracket the threshold, watched live rather than left to run blind — see `host/rig/io_monitor.py`-style pybela polling in this session's transcript), loop-gain sat at exactly 1.000 for ~20s then grew — a real, clean exponential climb, -30→-18.4 dBFS over ~11s. **Bracket: the true unity crossing is somewhere between 12 o'clock and 100%**, not narrowed further (Abel: no reason to expect an intermediate value tells us more, given how thin the margin is even at max). See `rig-profile.json` `loop.unity_gain_master_setting_note` and `logs/2026-09-13/`'s two feedback-ramp records.
6. Expression pedal: pot value, taper, TRS convention, heel/toe endpoints as read by the ADC — calibrate and store these, never assume a range.
7. Baseline recording: **the rig with no regulation at all**, feeding back at several volume-pedal positions. This is the "before" that every later result is compared against, and it is what winner-takes-all looks like in the metrics. Blocked on item 5.

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

**What Phase 3's actual exit criterion still needs, honestly:** a real jump, on a real rig, to measure milliseconds-to-flag against. It doesn't exist yet in any recording — the 2026-09-06 Mac-rig "second partial blooms" take (`logs/2026-09-06/gen1_cell_t24.wav`) was checked and is the *wrong kind* of event for this: D4's rise there is a slow, cell-release-mediated bloom (order 5–8 dB/s, over ~15 seconds), not the fast unity-crossing jump §7 describes (order 100–1000 dB/s) — the growth detector correctly does not arm on it, for the same reason it correctly ignores ordinary playing dynamics.

**Manufactured instead of waited for, 2026-09-13 (Abel's suggestion).** Rather than wait for a natural fast jump during play, `bela/detector-passthrough/` grew a host-controlled overall gain, stepped sharply above unity at a precisely-timestamped moment, to produce one on demand — DTA120 at 100%, two trials. Gain 0.2→5.0 produced fast, real growth (-86→-13..-18 dBFS in 3s) but `host/rig/analyse_take.py` showed it was broadband (`mean_partials=6.23`, `dominance=6.8dB`) — many partials crossing threshold at once, not a single one jumping; the detector correctly didn't arm, since that isn't the scenario it watches for. Gain 0.2→1.3 produced a genuinely clean single-partial event (`dominance=103.8dB`, `stability=0.1 cents`, 579.2 Hz) — the actual target scenario — and the detector *still* didn't arm, over 20s. Diagnosis: `kArmGrowthDbPerFrame` was set from this section's own theoretical 100–1000 dB/s estimate (for loop_gain≈1.4); this rig's real measured growth on a clean, unambiguous jump averaged only ~2.5 dB/s. Per CLAUDE.md rule 16's spirit (metric disagrees with a real, verified result → the metric is the bug), **the threshold is now the prime suspect, not the detector's logic or this test.** Not retuned yet — flagged here for whenever detection work resumes. Full data: `logs/2026-09-13/182636_detector-jump-latency-v3-dta120-100pct.json`.

Capturing/measuring a jump was the real open item; it's now been done, just with an unexpected result. Abel's call, 2026-09-13: move on to Phase 4 and come back to retuning this threshold later rather than block on it now.

**Follow-up sweep, same session:** six gain steps (1.0, 1.5, 2.0, 3.0, 5.0, 10.0) back-to-back to map the transition. Clean break: 1.0-1.5 grows slowly over seconds and never arms in an 8s window (matches the isolated 1.3 trial above); ≥2.0 grows explosively, arms in 45-84ms, and slams into the ceiling immediately. Caveat: only 2s settle between stages means the higher-gain stages likely re-amplify the previous stage's residual ringing rather than starting cold, which plausibly explains why 5.0/10.0 armed fast here but not in the isolated trial above — so this doesn't overturn the threshold-miscalibration finding, it sharpens it: the detector is fine once a jump is large, it's the realistic near-unity case it misses. Full data: `logs/2026-09-13/183505_jump-sweep-v3-dta120-100pct.json`.

**Exit:** detection latency after a jump is under the budget, with no false arming during ordinary playing. **Partly met:** no false arming is now demonstrated both synthetically and against real recorded audio; the latency-after-a-jump half needs a real jump, still to come.

---

## Phase 4 — One cell — the first musical milestone

*Simulator for tuning, Abel in the room for the verdict.*

A single gain cell: peaking-EQ biquad with negative gain, driven by an envelope follower targeting a level, fast attack / slow release, interpolated coefficients so it never clicks.

The success test is specific and it is audible: **hold the dominant partial at its target and a second partial should appear on its own.** That is the physics of §5 working. If the second partial does not appear, either the cell is too shallow (the winner still wins) or too deep (loop gain drops below unity everywhere and the feedback dies) — and the diagnosis is in the metrics, not in guesswork.

Tune attack, release, Q and target on the simulator; validate a shortlist on the rig; Abel rates.

**Built and run on hardware, 2026-09-13.** `bela/gen1-cell/` — compiles clean on this board's
toolchain, then run for real: DTA120=100%, Abel present, 30 s
(`logs/2026-09-13/185836_first-cell-take-dta120-100pct.json`). No mute, no watchdog trip, no
xrun/NaN. Untuned — SC's original defaults (target -24 dB, Q 10, attack 3 ms, release 400 ms,
max cut 30 dB), Phase 2's simulator still skipped so there's nothing to search against yet.
Binds its one cell to a partial via Phase 3's STFT/growth-detector machinery reused as an N=1
allocator, but by loudest-stable-peak rank rather than growth rate — see the file's header
comment for why (the growth-arm threshold flagged miscalibrated in Phase 3's status above
would likely never fire for this rig's real growth rates).

**Abel's verdict, listening to both files: pass.** "A nicely blooming chord, always 3-4 notes
present" — more than the minimum one extra partial the exit test asks for. From ~16s onward
some distortion is audible; not yet attributed to one specific cause — either the DSP output
ceiling (peak_sample did hit 0.5 in this take) or the exciter itself, which turns out **not
to be fixed/mounted** yet (just resting on the guitar body, can physically "jump" and distort
under heavy drive independent of the DSP — see `rig-profile.json` `exciter._mounting_note_2026-09-13`,
a genuinely new fact about the rig, not a change made mid-session). Abel's call: proceed
regardless (CLAUDE.md rule 16 — his ear is ground truth) and revisit distortion-source once it
matters more (mounting the exciter properly would also just remove one whole axis of ambiguity
from every future take).

**Exit:** on the real rig, a second partial reliably blooms, and feedback sustains rather than dying. **Met, 2026-09-13.**

---

## Phase 5 — Multi-cell, allocator, and the jump case

*Mostly agent, in the simulator; periodic real-rig validation sessions.*

- Cell pool N = 4, then 6–8. Allocator per §6.2: arm on growth score, ±50 cent glide window, release with ramp-back, anti-chatter hold and lockout, steal-least-active when full.
- **Jump handling as an explicit test case**, not a hope: a scripted simulator scenario where the loop's favoured mode is switched abruptly, scored on time-to-regulate and on whether the incumbent's cell releases cleanly.
- Optional here, not before: the adaptive-notch fine tracker under FFT supervision (§6.4), evaluated as an A/B against plain interpolated biquads.
- Watch CPU load as N grows.

**Built, 2026-09-13, not yet run on hardware.** `bela/gen1-multicell/` generalises
`bela/gen1-cell/` (Phase 4, PASSED) from one cell to N=4, cascaded in series on the audio
path, each with its own bandpass detector + envelope follower + peaking-EQ actuator. Compiles
clean on this board's toolchain (compile-only check, never run). Carries forward gen1-cell's
two deliberate divergences from ground-rules 6.2/6.3 and sc/gen1_cell.scd (bind by
loudest-stable-peak rank, not growth rate; one envelope-follower stage) for the same reason —
see that file's header. New in this file: **steal-least-active** (a candidate left over with
no free cell takes the currently-BOUND cell with the smallest current cut, past its own
anti-chatter hold — the frequency/gain glide there for free via the existing per-sample slew,
no special-cased crossfade needed), a **lockout** on a cell's just-vacated bin after release or
steal (ground-rules 6.2), and **CPU-load monitoring** wired to Watcher via Bela's own
`Bela_cpuMonitoringInit/Get` (this phase's "watch CPU load as N grows" line, actually measured
rather than left as a TODO). Not done: growth-rate-based arming (deferred with the same
reasoning as gen1-cell), the adaptive-notch fine tracker (explicitly optional here), and the
formal jump-handling test case — steal-least-active is exercised only implicitly so far, not
yet validated against a manufactured jump the way Phase 3's detector was.

**Run on hardware, 2026-09-13 (same day, after the exciter was secured): two live-playing
takes read much closer to single-partial than gen1-cell's pass** — partial_count 1.2-1.4 vs
6.36, peak_sample only 0.07-0.15 vs 0.5 (`logs/2026-09-13/{192844,193307}_*multicell-take*`).
Investigated three ways, all Abel's suggestions, all agent-run:

1. **`host/rig/snapshot_partials.py`** (new — time-windowed re-scoring of a recording instead
   of one whole-take average). Applied to the 40s take: it WAS trending toward multi-partial,
   just slowly — dominance drops from 120 dB to 1.1 dB over the last ~15s. The aggregate mean
   had washed that out completely.
2. **A clean engaged/disengaged A/B** (new: a 2s/150-500Hz/0.05-linear sweep-kick seeds every
   run automatically, and a Watcher-settable `bypass` lets the exact same code run with the
   cells computed-but-not-applied — see `bela/gen1-multicell/render.cpp`'s header). Same
   sweep, same 40s, only `bypass` differs: **disengaged** builds up transiently then decays
   back toward one dominant partial (dominance climbing back to 21 dB by the end) — textbook
   winner-takes-all reasserting itself. **Engaged** locks onto a stable 4-partial texture by
   ~15s and holds it for the rest of the take, dominance 2.4-4.6 dB throughout
   (`logs/2026-09-13/200034_ab-disengaged*`, `200241_ab-engaged*`). This is the clearest
   positive evidence yet that the allocator does what it's supposed to.
3. **`host/harness/multicell_sandbox.py`** (new — offline, open-loop, synthetic sine tones
   through a Python reimplementation of the same allocator/actuator logic; NOT Phase 2's loop
   simulator, no feedback at all, can't test blooming, only tests whether the code's decisions
   are sane). Validated: single-tone regulation lands within ~0.2 dB of target; four
   well-separated equal tones all bind and regulate correctly; a quiet tone under target gets
   0 dB cut (the "does nothing at or below target" invariant holds); a later, louder, distant
   5th tone correctly triggers exactly one steal. **One real finding, not fixed**: five equally
   loud tones spaced 120 Hz apart → only four ever bind, the fifth is permanently excluded
   (never bound, never stolen for) by `kExclusionBinRadius=6` bins (~129 Hz at this
   window/hop). Whether that's too wide depends on the guitar's actual partial spacing —
   ground-rules §7.3's transfer-function measurement (still unmeasured) would settle it, so
   this is flagged for whenever that measurement happens, not guessed at now.

Two live single-note takes underperforming a controlled synthetic sweep is at least
consistent with (not proven by) that same exclusion-radius question: a plucked note's own
lower harmonics can sit closer together than 129 Hz, which the sweep-kick test's
well-separated tones didn't exercise.

**Same day, three more requests answered in one pass**, ahead of a live-playing session:

- **N bumped 4 → 12.** Nothing in the allocator/actuator assumed N=4; the only real question
  was CPU headroom, checked live rather than assumed — a periodic `rt_printf` of
  `Bela_cpuMonitoringGet()` read ~32% audio-thread CPU at N=12 (a units bug in that same
  reporting — `BelaCpuData::percentage` is already 0-100 — was caught and fixed along the way;
  it had first read as ~3245%).
- **Steal made click-safe.** The frequency slew alone glides a stolen (or same-hop-frame
  released-and-rebound) cell's centre smoothly, but it can still carry a substantial cut from
  its OLD partial while that glide happens — sweeping an audible notch across every frequency
  in between, not a click exactly but exactly the "loud sudden change" this was asked to rule
  out. Fixed with a "rebind duck": a per-cell epoch counter, bumped on every fresh bind or
  steal, tells the audio thread to ramp the *applied* cut (not the computed one — telemetry
  and the steal decision itself still see the real value) down to 0 and back over 50ms,
  deliberately slower than the 20ms frequency slew so the notch is essentially gone before the
  frequency has finished moving.
- **`bela/gen1-multicell-live/`** — same N=12, same duck-safe allocator, sweep-kick removed
  entirely (opposite of what a live session wants) rather than merely disabled. The browser
  GUI looked genuinely, reproducibly broken through an extensive investigation (three theories
  chased and ruled out with real evidence — Watcher needing Bela's `dev` branch, Chrome's
  Local Network Access policy, a non-standard HTTP handshake reason phrase — see git history
  if it matters later) — **and then it just worked in Abel's actual Edge browser.** Never
  fully explained; working theory is the connection's own retry loop needed more patience
  than this session's quick automated checks ever gave it. Along the way: found that
  Watcher's upstream repo ships a `sketch.js` (p5.js frontend) never vendored into any project
  here — now is, and replaced with a **custom view**: a 12-cell grid plus a summary row,
  instead of the generic 53-row variable table. **Added same day: five live tuning sliders**
  (target level, max cut, release time, cell Q, a new software loop-gain multiplier) **plus a
  bypass checkbox**, per Abel's request to start tuning the suppression effect without
  recompiling — all Watcher-controllable from the browser, all clamped in code regardless of
  what's sent, verified end to end with a raw websocket test (set `target_db`, read back the
  changed value, not assumed working). Found a second real bug while wiring these up: a
  Watcher variable needs an explicit `.localControl(false)` call to actually be settable
  remotely — the existing `bypass` variable had the same latent gap despite being named like
  it was controllable; fixed here and ported back to `gen1-multicell`. `host/rig/
  multicell_monitor.py` remains available as a terminal fallback if the browser view flakes
  again.
- **Found and fixed a second bug along the way**: pybela's own streaming (Python-side, the
  mechanism Phase 1 already confirmed working) was *also* failing, for an unrelated reason —
  `'NoneType' object has no attribute 'groups'` from inside pybela's own websocket handling.
  Traced to every project here except `bela/watcher-check/` writing its Watcher telemetry once
  per render() *block* rather than every audio *sample* — confirmed by moving
  `detector-passthrough`'s Watcher writes into its per-sample loop and watching the same pybela
  call start working. Fixed at the source in `gen1-multicell`/`gen1-multicell-live`'s
  `render()` (redundant per-sample writes of an already-known block-level value, e.g. a peak
  accumulator, are harmless); flagged as a follow-up for the four older projects that still
  have the once-per-block pattern, now bundled with the `localControl(false)` fix above into
  one combined follow-up task.
- **`host/rig/multicell_monitor.py`** (new) — a live terminal view over pybela, useful as a
  fallback if the browser view ever flakes again: per-cell bound/freq/cut/level, bind/
  release/steal totals, in/out peak, CPU%, redrawn a few times a second while a project runs.

Both variants compile clean; `gen1-multicell` was run briefly (foreground, to read the real
CPU%) and had to be stopped explicitly when a local `timeout` around `ssh -t` didn't propagate
to the remote process tree — under a minute total regardless (the board's clock being UTC, 2h
off local, made it look longer for a moment), and the 120s watchdog would have caught it either
way, but `host/rig/*_run.py`'s background+`stop_running.sh` pattern is the one to actually rely
on, not a foreground timeout. `multicell_monitor.py` itself confirmed working end to end against
a running `gen1-multicell-live` (no signal playing, so nothing bound — the plumbing is what was
being checked).

**Exit:** 3+ simultaneous partials sustained in the simulator *and* on the rig; jump regulation inside the latency budget; no chatter, no clicks, no runaway. **Partly met**: 4 simultaneous partials sustained on the real rig for 30+s under the sweep-kick A/B above — but that was a synthetic seed signal, not live playing, and the simulator half still doesn't exist (Phase 2 still deliberately skipped). **A live-playing session on `gen1-multicell-live`, watched via `multicell_monitor.py`, is the next real-loop thing to do** — the tooling is confirmed working, nobody's played through it yet.

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
| Passive pickup into an unbuffered board input degrades detection | Was covered by the M4's buffered instrument input; the M4 was removed 2026-09-13 (ground loop, see ground-rules §4.4) and the guitar runs direct/unbuffered into Bela for now, by deliberate choice, until a passive DI box (chosen specifically to restore buffering *and* keep the ground isolation the M4 accidentally broke) arrives |
| Uncontrolled acoustic loop through the amp masks everything | No amp is wired into this build at all during development; reintroduced only in Phase 7, at which point ground-rules §4.5's "no hardware kill needed" reasoning must be revisited |
| CPU ceiling on PocketBeagle 2 | Load tracked from Phase 3 on; Goertzel bank is cheap by design; N is a tunable |
