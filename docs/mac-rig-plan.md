# Mac + Focusrite rig — the simple plan

**Supersedes `phase-plan.md` for the time being.** The Bela is not connectable, so gen1 moves
to a Mac host with the Scarlett 8i6 as the audio interface. Everything in
`ground-rules-and-facts.md` about *the physics, the DSP architecture and what "good" means*
still stands unchanged. What changes is the platform, the latency budget, the control
surface and the safety hardware. Those changes are recorded in §3.3 and §4.6 of the
ground-rules doc — this file does not silently contradict it.

Drafted 2026-09-06. Status: M0 partly measured — see §3 and §4.

---

## 1. The rig

```
   guitar ──► Scarlett 8i6 INPUT 1 (INST / Hi-Z) ──► Mac  ──► Scarlett OUTPUT 3/4
      ▲                                            (Python DSP)          │
      │                                                                  │
      │                                                          [ ATTENUATOR ]   ← the kill
      │                                                                  │
   pickup ◄── strings ◄── body ◄── EXCITER ◄── power amp ◄───────────────┘

                      THE CONTROLLED LOOP — the only loop that is live
```

Once input 1 is fed to output 3/4 with enough gain, the loop is closed and it feeds back.
That is the whole bring-up test.

**What the new rig fixes for free.** The Scarlett's INST input is high-impedance, so
ground-rule §4.1 — "never drive Bela's ~20 kΩ ADC with a passive pickup" — is satisfied by
the hardware. No external buffer needed.

**What the new rig costs.** Round-trip latency goes from Bela's ~1 ms to roughly 25 ms of
configured buffering (11 ms in + 14 ms out, measured 2026-09-06 — and only after explicitly
asking PortAudio for low latency; see §3). This is not fatal — latency
sets *which* partials satisfy the 360°·n phase condition, i.e. the spacing of the comb, not
whether feedback happens. It does tighten the jump-detection budget of §7. Measure it in
M0 and treat the number as a fact, not a nuisance.

**The guitar amp stays silent**, exactly as before. Only the controlled loop is live.

---

## 2. Safety on this rig — read before the amp is switched on

**Settled with Abel, 2026-09-06.** The answers to the open questions this section used to
carry:

- **Routing confirmed.** Outputs 3/4 are fed from Playback 3/4, and leave the interface via
  the **second headphone jack**, which drives the power amp. No hardware mix carries Input 1
  to those outputs. Re-check every session anyway; it is one click away from being wrong.
- **There is no volume pedal.** The designated kill is therefore the **headphone-2 level
  knob** — it is on the desk, which makes it a better kill than reaching for the amp — with
  the power amp's own control as the backstop. Abel operates it; the software has no
  physical authority over level.
- **Sustained unchecked feedback is accepted**, explicitly, because *only the controlled
  loop is live*: the guitar-amp path is dead, so the runaway acoustic loop of §2.3 cannot
  form. A run does not have to be killed between rounds.

That last permission is conditional and worth stating as a condition rather than a fact:
**it holds only while the guitar amp is dead.** Switch the amp on and the second,
uncontrolled loop is back, the reason for the permission is gone, and the strict form of
ground rule 2 applies again.

What stays regardless:

1. **Hard output ceiling in the DSP.** A constant. No tuning process may raise it —
   ground rule 1, unchanged, and it is why the ceiling code is not delegated to Cursor.
   Abel accepting loud feedback is not the same as letting an optimiser choose the level.
2. **Watchdog.** Every live-loop run is time-boxed. Generous now rather than tight, since
   a long run is fine — but a run can never outlive the session that started it.
3. **Mute on anything odd** — NaN, non-finite sample, callback exception, stream stop.
   Never "recover and keep going".
4. **Every session starts with the amp down** and the loop gain ramped up from silence.
   Never start a run at the last session's setting.
5. Real-loop runs happen only with Abel present and confirming, per ground rule 2.

---

## 3. Software stack, and why

**Python 3 + `sounddevice` (PortAudio/CoreAudio) + NumPy + SciPy. One process.**

The DSP runs in the audio callback: 48 kHz, 128-frame blocks (2.67 ms). Analysis is a
2048-point `rfft` per block — about 375 FFTs/second of a size NumPy does in tens of
microseconds. There is no need for the separate auxiliary analysis task the Bela design
required, which removes a whole layer of machinery.

Why this and not the alternatives:

| Option | Verdict |
|---|---|
| **Python + sounddevice** | Chosen. Same language as the existing harness and metrics. Zero deploy step — edit, run, hear. The agent can iterate without a cross-compiler. |
| Pd / SuperCollider | Another toolchain and another language for the harness to talk to, for no gain here. |
| JUCE / C++ plugin | The right answer for a product, far too heavy for finding out whether the idea works. |
| Bela | Unavailable. The port back is easy later — the DSP is a few hundred lines of arithmetic. |

**Measured, 2026-09-06, before any of this was tuned:** 60 s duplex at 48 kHz / 128 frames,
**zero xruns**, callback worst case **0.084 ms against a 2.67 ms deadline** — about 3% of
the budget. Python is not the bottleneck. But the first run also exposed a trap worth
recording: `sounddevice` defaults to `latency="high"`, and on this interface that asked for
**~90 ms of buffering in each direction**. Asking for `"low"` brings it to ~11 ms in /
~14 ms out. In a feedback loop that difference is not comfort — loop delay sets which
partials satisfy the 360°·n phase condition, so 180 ms round trip would pack the comb teeth
about 5 Hz apart and put the §7 jump budget out of reach. **Every stream in this project
asks for low latency explicitly.** The number that actually counts is still the electrical
loopback measurement, which needs a cable.

**The known risk is Python, honestly stated:** garbage collection or an allocation in the
callback causes a dropout, and a dropout inside a feedback loop is a click the loop then
amplifies. Mitigations, all cheap: preallocate every buffer at startup, no allocation and
no logging inside the callback, `gc.disable()` for the duration of a run, count and report
xruns, and treat a nonzero xrun count as a failed run rather than a quirk. If M0 cannot
hold 60 seconds at 128 frames with zero xruns, drop to 256 frames; if that still fails,
the escape hatch is a small C PortAudio host with the same parameter file. Decide on
measurement, not on nerves.

**Per-block, not per-sample.** Cell gains are updated once per block and one-pole smoothed,
so coefficients never jump. 2.67 ms of granularity sits inside the 1–5 ms attack budget of
§6.3, so nothing musical is lost. This is the single biggest simplification versus the Bela
design and it is a legitimate one.

---

## 4. Phases

Five, not eight. Each states what gets built, who is needed, and the exit criterion.

### M0 — I/O bring-up · *agent alone; amp down, not off*

- `host/rig/io_check.py`: open the Scarlett at 48 kHz / 128 frames, meter input 1 with the
  guitar plugged in, count xruns over 60 s.
- **Channel map, confirmed by observation.** `io_check map --out N` puts a quiet tone on one
  output and watches the input RMS. On the Bela rig that needed a loopback cable; here it
  does not, because **the exciter already closes the path** — a tone on the right output
  goes amp → exciter → body → strings → pickup → input 1, and input 1 lifts. So the same
  command confirms the channel index *and* proves the whole physical chain in one move, at
  a tone amplitude of 0.05 with the amp turned well down. A wrong index shows nothing.
- Set input gain so normal playing peaks around −12 dBFS, leaving headroom for the loop
  to grow into.
- Round-trip latency: **optional here.** Abel has deprioritised it (§7) — this rig is for
  testing. `io_check latency` and a cable remain available if the comb spacing ever matters.

**Exit:** the stream opens reproducibly, 60 s at 128 frames with zero xruns, the channel map
is confirmed by observation, input gain is staged.

*Status: xruns and callback headroom are done and clean (§3). The channel map and the gain
staging need the guitar plugged in and Abel's hand on the headphone-2 knob.*

### M1 — close the loop and confirm feedback · *Abel present, this is the milestone*

- `host/rig/loop.py`: input 1 → software master gain → hard ceiling → outputs 3/4. Plus
  panic key, watchdog, mute-on-error, xrun counter, and recording of both input and output.
- Amp from zero. Raise loop gain slowly until the loop reaches unity. **Write down the
  gain at which it starts, and which partial it picks.**
- Record baseline takes at three positions above unity: just-unity, comfortable, hot.
- Run `harness.metrics.analyse` on them. This is the first time those metrics see real
  audio, and it is as much a test of the metrics as of the rig. Expect them to say:
  one partial, high dominance ratio, low flatness. If they do not say that, the metric is
  the bug (ground rule 12).

**Exit:** feedback sustains, is reproducible, and the baseline recordings measurably show
winner-takes-all. This is the "before" every later result is compared against.

### M2 — detection only, actuator at unity · *agent-heavy*

- FFT peak picking with quadratic interpolation, plus per-bin magnitude, **growth rate**,
  narrowness and persistence (§6.1). Gain cells present but at unity — the audio is
  untouched, so this is safe to run live.
- Validate offline against the M1 recordings **first**; only then run it live. Offline
  validation costs no rig time and no Abel time.
- The number that matters: after the loop jumps to a new partial, how many milliseconds
  until the growth scorer flags it.

**Exit:** the detector finds the partials the M1 baselines actually fed back on, and flags
a new one by its growth rate while it is still well below the incumbent — not after it has
already won.

### M3 — one adaptive cell · *the design Abel asked for* — **PASSED 2026-09-06**

Built in SuperCollider instead of Python (`sc/gen1_cell.scd`), on Abel's instruction.
Run unattended. Results, all four takes logged in `logs/2026-09-06/`:

**The baseline.** The loop reaches unity at master gain ≈0.30 and locks on **G4, 392.5 Hz,
stable to 0.4 cents**. The next partial is **25.7 dB down**. Mean partials 1.94, and at the
10th percentile exactly **one**. That is winner-takes-all, measured.

**With one cell, target −24 dB.** Same gain, same 8-second window:

| | loudest | runner-up | gap | dominance | mean partials | sustained |
|---|---|---|---|---|---|---|
| bypassed | G4 | G5 −25.7 | **25.7 dB** | 55.9 dB | 1.94 | 0.41 |
| peaking EQ | D4 | G4 −2.8 | **2.8 dB** | 49.5 dB | 1.90 | 0.67 |
| band-reject | D4 | G4 −7.2 | **7.2 dB** | **33.9 dB** | **2.53** | 0.65 |

D4 rose about 23 dB and now coexists with G4. **The feedback did not die — it sustains
more** (0.41 → 0.67), which rules out the "too deep" failure mode. The telemetry shows the
regulator behaving exactly as designed: it held G4 at target with a **steady −4 dB** cut for
twelve seconds, applying the minimum reduction that did the job rather than a fixed depth.

The band-reject shape Abel asked for beats the peaking EQ on the aggregate metrics, which
was not the expected result — the ground rules make the peaking EQ the default. Worth a
proper A/B before either is made the default.

**The defect this run exposed, which matters more than the pass.** The detector is
`Pitch.kr`, autocorrelation pitch tracking. It is fine while exactly one partial is
running away, and it breaks *precisely when the cell starts working*: in the two-partial
regime the real energy is at 293.5 and 392.5 Hz, and the tracker reports **~330 Hz, where
there is no energy at all**. The cell then regulates nothing. So the improved two-partial
state is a resting point the loop happened to find, not a state the cell is actively
holding. Pitch detection finds a fundamental; we need the *dominant partial*, which is a
different question. Replacing it with the FFT peak picking of §6.1 is now the first task
of M4 — and that is what the multi-cell allocator needs anyway.

---

Original specification, for reference:

One cell, bound to the dominant partial:

- **peaking-EQ biquad with negative gain**, centre frequency from the detector,
  slew-limited; Q fixed, start around 10.
- gain driven by an **envelope follower comparing that partial's level to a target** —
  fast attack (1–5 ms), slow release (100 ms – 2 s), one-pole smoothed per block so the
  coefficients never step.
- on release the reduction **ramps back**, never resets instantly.

A note on the word *notch*: what gets built is the level-targeting gain cell of ground-rule
§5, not a fixed-depth notch. A fixed notch is either too shallow to stop the winner or deep
enough to kill the note. The target is a level; the depth is whatever it takes to hold it.
"Adaptive notch" is fine as shorthand for the frequency tracking; the actuator is a
regulator.

**The success test is audible and specific:** hold the dominant partial at its target and a
**second partial should appear on its own**. That is the inhomogeneous-saturation argument
of §5 working, or not working. If the second partial never comes, the cell is too shallow
(the winner still wins) or too deep (loop gain fell below unity everywhere and the feedback
died) — and the diagnosis is in the metrics, not in guesswork.

**Exit:** on the real rig, a second partial reliably blooms and the feedback sustains.

### M4 — N cells, allocator, and a control · *agent for the code, Abel for the verdict*

- Cell pool N = 4, then 6–8. Allocator per §6.2: arm on growth score, ±50 cent glide
  window, ramp-back release, anti-chatter hold and lockout, steal-least-active when full.
- The jump case as an explicit scripted test, not a hope.
- **Control surface stand-in.** There is no analog input on this rig, so regulation depth
  is a MIDI CC (any controller, or a phone) or a keyboard key, mapped to the per-cell
  target level exactly as the expression pedal was. Same axis, different transducer; the
  mapping work carries over to the pedal later unchanged.
- Watch CPU and xruns as N grows.

**Exit:** 3+ simultaneous partials sustained on the rig, jumps regulated inside the
latency budget, no chatter, no clicks, no runaway.

---

## 5. What this plan drops, and what that costs

| Dropped from the Bela plan | Why it is safe to drop | What it costs |
|---|---|---|
| Cross-compile / `build_project.sh` / deploy | No board | Nothing. This was pure overhead. |
| pybela `Watcher` streaming | The DSP and the harness are now the same process; internal state is a Python variable | Nothing. This is a large simplification. |
| Buffered-split requirement (§4.1) | The Scarlett INST input is high-impedance | Nothing. |
| Expression pedal + analog-in calibration | No analog inputs | The foot control, deferred to M4 as MIDI/keyboard. The *mapping* work is unaffected. |
| **Loop simulator as a gate** (old Phase 2) | Iteration on the real rig is now cheap — no deploy step — so the simulator is no longer the autonomy unlock it was | Overnight parameter sweeps. Keep the simulator as an M4 option, not an M0–M3 prerequisite. **This narrows ground rule 8 and needs Abel's explicit sign-off — see §7.** |

Not dropped, and not negotiable: the output ceiling, the watchdog, mute-on-error, one
variable at a time, every run logged with its parameter set, Abel's ratings outranking the
metrics.

---

## 6. Division of labour — Claude, Cursor, Abel

Cursor is driven through the `cursor-agent` CLI, which is installed on this machine.

**Cursor gets** work that is mechanical, fully specified in advance, and verifiable by a
test that already exists: the device-enumeration and metering script, the recorder, the log
record writer, the parameter-file plumbing, unit tests, mechanical refactors.

**Claude keeps** the DSP design decisions, the detector and cell maths, the metrics, the
docs — and, explicitly, **every safety-critical path: the output ceiling, the watchdog, the
mute paths.** Ground rule 1 says the ceiling changes only by an explicit human commit;
routing it through a second agent adds a hop where it can drift, for no benefit.

**Abel does** everything physical, everything with ears, and every go/no-go on a live loop.

---

## 7. Open questions — answer by measurement, or by Abel

**Answered, 2026-09-06:**

- ~~Is the volume pedal going back in?~~ No pedal. The kill is the headphone-2 level knob,
  Abel's hand on it. See §2.
- ~~Focusrite routing?~~ Playback 3/4 → headphone 2 → power amp. Confirmed, no hardware mix.
- ~~Can Python hold 128 frames with zero xruns?~~ Yes. 60.00 s, 22500 blocks, zero xruns,
  callback worst case 0.062 ms against a 2.67 ms deadline. See §3.
- ~~Round-trip latency and comb spacing?~~ **Deprioritised by Abel** — this rig is for
  testing and latency is not a concern here. The electrical loopback measurement is
  therefore optional, not an M0 exit criterion. PortAudio reports ~25 ms of configured
  buffering; if a partial-selection question ever turns on the exact comb spacing, measure
  it then with `io_check latency` and a cable.

**Still open:**

1. Which guitar, which tuning, which pickup. *Needed for the candidate list.*
2. Where the exciter sits on the body, and whether it drives the strings or mainly the top
   — this decides whether feedback locks to string partials or body modes. *M1.*
3. **Does Abel accept narrowing ground rule 12** — simulator-first for parameter *searches*,
   but a single-hypothesis A/B may now go straight to the rig, since the deploy step that
   made rig time expensive is gone? *Blocking for M4, not before.*
4. Does the loop restart cleanly from silence, or does the guitar need damping between
   takes?
5. Which output index is really "outputs 3/4"? Still unconfirmed by observation — but on
   this rig the confirmation is free, because the exciter closes the path back to input 1.
   See M0's revised channel-map step in §4.

---

## 8. Repo naming

The repo is still called `bela-feedback-pedal` and the Bela sources stay in `bela/`
untouched. Renaming buys nothing and breaks every existing path; the Bela is a deferred
target, not a cancelled one, and the DSP written here ports to it directly.
