# Mac + Focusrite rig — the simple plan

**Supersedes `phase-plan.md` for the time being.** The Bela is not connectable, so gen1 moves
to a Mac host with the Scarlett 8i6 as the audio interface. Everything in
`ground-rules-and-facts.md` about *the physics, the DSP architecture and what "good" means*
still stands unchanged. What changes is the platform, the latency budget, the control
surface and the safety hardware. Those changes are recorded in §3.3 and §4.6 of the
ground-rules doc — this file does not silently contradict it.

Drafted 2026-09-06. Status: not yet run.

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

**What the new rig costs.** Round-trip latency goes from Bela's ~1 ms to something in the
5–15 ms range (USB + CoreAudio double-buffering + converters). This is not fatal — latency
sets *which* partials satisfy the 360°·n phase condition, i.e. the spacing of the comb, not
whether feedback happens. It does tighten the jump-detection budget of §7. Measure it in
M0 and treat the number as a fact, not a nuisance.

**The guitar amp stays silent**, exactly as before. Only the controlled loop is live.

---

## 2. Safety on this rig — read before the amp is switched on

The old primary kill was the volume pedal at heel. That device is gone from the chain
unless we put it back, and the replacement is **not** the Scarlett's big monitor knob:

> **On the 8i6, outputs 3/4 are fixed-level line outputs. The front-panel monitor knob
> attenuates outputs 1/2 only. Turning it down does nothing to the exciter.**
> Verify this on the actual unit in M0 before trusting it either way.

So:

1. **Put the volume pedal back, between Scarlett out 3/4 and the power amp.** It costs
   nothing, it restores the primary hardware kill, and it restores the loop-gain control
   that the whole control surface of §8 is built around. If the pedal is not used, the
   power amp's own volume control must be within arm's reach and is the designated kill.
2. **Focusrite Control: outputs 3/4 must be fed from "Playback 3/4" only.** Not from a
   hardware mix containing Input 1, and Direct Monitor off. A hardware-mix path would close
   an *analog* loop inside the interface that the DSP cannot see, cannot regulate and
   cannot mute. Check this every session; it is one mouse-click away from being wrong.
3. **Hard output ceiling in the DSP.** A constant. No tuning process may raise it —
   ground rule 1, unchanged, and it is why the ceiling code is not delegated to Cursor.
4. **Watchdog.** Every live-loop run is time-boxed. No heartbeat, no run: it mutes.
5. **Mute on anything odd** — NaN, non-finite sample, xrun burst, callback exception,
   stream stop. Never "recover and keep going".
6. **Every session starts with the amp at zero** and the loop gain ramped up from silence.
   Never start a run at the last session's setting.
7. Real-loop runs happen only with Abel present and confirming, per ground rule 2.

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

### M0 — I/O bring-up · *agent alone, amp OFF*

Nothing drives the exciter in this phase. The amp is off or unplugged.

- `host/rig/io_check.py`: open the Scarlett at 48 kHz / 128 frames, confirm the channel map
  by measurement (send a tone to one output at a time and see where it appears — do not
  trust that "output 3/4" is index 2/3 until it is observed), meter input 1 with the guitar
  plugged in, count xruns over 60 s.
- **Round-trip latency, measured electrically:** patch output 3 back into a line input,
  send an impulse, find the sample offset. That is the real number for the phase condition.
- Set input gain so normal playing peaks around −12 dBFS, leaving headroom for the loop
  to grow into.
- Write `rig-profile.json` v1 for this rig.

**Exit:** the stream opens reproducibly, 60 s at 128 frames with zero xruns, the channel
map is confirmed by observation, the round-trip latency is a number in the profile.

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

### M3 — one adaptive cell · *the design Abel asked for*

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

1. **Is the volume pedal going back into the chain between out 3/4 and the amp?** If not,
   name the designated hardware kill. *Blocking for M1.*
2. Does the Scarlett's monitor knob attenuate outputs 3/4 on this unit? *Measure in M0.*
3. Measured round-trip latency at 128 frames, and therefore the comb spacing. *M0.*
4. Can Python hold 128 frames with zero xruns for a full take? *M0.*
5. Which guitar, which tuning, which pickup. *Needed for the candidate list.*
6. Where the exciter sits on the body, and whether it drives the strings or mainly the top
   — this decides whether feedback locks to string partials or body modes. *M1.*
7. **Does Abel accept narrowing ground rule 8** — simulator-first for parameter *searches*,
   but a single-hypothesis A/B may now go straight to the rig, since the deploy step that
   made rig time expensive is gone? *Blocking for M4, not before.*
8. Does the loop restart cleanly from silence, or does the guitar need damping between
   takes?

---

## 8. Repo naming

The repo is still called `bela-feedback-pedal` and the Bela sources stay in `bela/`
untouched. Renaming buys nothing and breaks every existing path; the Bela is a deferred
target, not a cancelled one, and the DSP written here ports to it directly.
