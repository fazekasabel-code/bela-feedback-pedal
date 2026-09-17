# Gen1 Feedback Regulator — Ground Rules & Facts

**Project:** Lithopaedion / Feedback Unbound DSP pedal, generation 1
**Platform:** Bela Gem Stereo (Starter Kit, PocketBeagle 2 base) — see §3.1.
**Owner:** Abel Fazekas · **Status:** pre-bring-up. Board on hand since 2026-09-10, not yet set up.
**Purpose of this doc:** the shared, stable reference every later decision and every agent session is checked against. Facts here are either measured, cited, or explicitly marked as assumptions to be tested. Nothing downstream should silently contradict it.

---

## 1. What gen1 is

**Goal.** A pedal that suppresses the winner-takes-all character of guitar feedback and instead produces rich, complex, multi-partial feedback tones.

**Mechanism in one sentence.** Instead of letting one runaway partial saturate the whole loop and starve every other mode, gen1 gives *each* dominant partial its own gain-regulation cell, so each one settles at a target level and leaves loop headroom for the next partial to bloom.

**Explicitly out of scope for gen1:**

- **Upward compression.** Deferred by decision. Rationale (Abel): in the guitar setup the signal's top level is already kept in check, and the distortion in the chain performs compression anyway. It is more needed on the flute rig than here.
- Pitch/harmony processing, reverb, any "effect" beyond the regulator.
- Enclosure, PCB, product form factor. Gen1 is a working Bela in a box with jacks.
- Multi-preset UI. One pedal, one expression input, at most a footswitch.

**Definition of done for gen1:** a rig that Abel can plug in, switch on, and play, where feedback sustains indefinitely on 3+ simultaneous partials, the mode does not collapse to a single screaming tone, jumps between notes are tracked within the target latency, and the expression pedal moves continuously between "raw winner-takes-all" and "dense flattened chorus of partials."

---

## 2. Signal topology

```
                       ┌──────────► [ effects ] ──► [ GUITAR AMP ]   ← acoustic output
                       │                                   ╎          NOT WIRED in this
   GUITAR ──► [ BELA in, DIRECT — no buffer ]               ╎          build — see §2.3
                       │                                   ╎  (uncontrolled
                       │                                   ╎   acoustic loop)
                       └──► DSP ──► [ BELA out ]
                                                        │
                                             [ power amp: Dayton DTA120 ]
                                                        │
                                              [ EXCITER: Dayton, on body ]
                                                        │
                                        body ──► strings ──► pickup ──┐
                                                                      │
                       ◄──────────────────────────────────────────────┘
                                    THE CONTROLLED LOOP

        [ expression pedal ] ──► Bela analog in 0   ← regulation depth (foot)
```

As actually wired (2026-09-11) there is no volume pedal and no other device between Bela
out and the power amp — see §4.5 for why that's a considered decision, not a gap. Loop gain
is presently whatever the DTA120's own gain control is set to, fixed per ground rule 10.5.

**Revised 2026-09-13: M4 removed from the chain, temporarily unbuffered.** The M4 (buffered
instrument input, direct out) shown in this doc's history is **no longer in the signal
path**. It was pulled after being identified as the source of a real ground loop (see §4.4)
— it shared USB power/data with the same Mac used to tether Bela, while also being linked to
Bela by an analog cable, closing a loop through two ground paths at once. The guitar now
plugs **directly** into Bela's audio in, unbuffered, mono — see §4.1's rule, which this
directly contradicts on purpose, not silently. A passive DI box is on order to restore
buffering (and, as a transformer, galvanic isolation) once it arrives; until then, high-end
roll-off from the impedance mismatch is a known, accepted cost, not a bug to chase.

### 2.1 The two chains are not symmetric

Only one of them is a loop.

- **Amplification chain** (guitar → effects → amp) is an *output*. It does not close a loop through the DSP. In this build it is not wired at all (§2.3).
- **Feedback chain** (pickup → Bela in, direct/unbuffered as of 2026-09-13 → DSP → Bela out → power amp (Dayton DTA120) → exciter (Dayton) → body → strings → pickup) is the **controlled loop**. This is the only loop that is live at all. Everything gen1 does happens inside it.

### 2.2 Split point: before or after the effects

Both are electrically viable. They are not musically equivalent.

| | Split **before** effects (recommended default) | Split **after** effects |
|---|---|---|
| What Bela sees | Clean pickup signal | Distorted / compressed signal |
| Detection quality | **Good** — partials are clean, peak picking is reliable | Harder — distortion adds harmonics and intermodulation products that look like partials |
| Feedback tone | Excitation is clean; the character comes from body + regulator | Exciter reinforces exactly the tone the amp is making |
| Independence | Feedback behaviour does not change when you change your amp settings | Feedback behaviour is coupled to every pedal change |

**Ground rule:** develop on the **pre-effects split**. Making it switchable is a gen1.5 nicety, not a gen1 requirement. If the post-effects tap is ever used, detection thresholds must be re-tuned — treat it as a different rig profile.

### 2.3 The amp is a second, uncontrolled loop

At stage volume, an amp's acoustic output excites the guitar body and strings directly. That is a real feedback loop, it is *not* under the pedal's control, and it has exactly the winner-takes-all character gen1 is trying to defeat. It will mask or undo the regulator's work.

**As actually built (2026-09-11, wiring detail updated 2026-09-13): this loop does not exist.** The guitar goes only into Bela's audio in (directly as of 2026-09-13, previously via the M4's buffered instrument input) and from there into the DSP; there is no live guitar amp or PA anywhere in the signal path. This isn't the amp being kept silent by discipline — it simply isn't wired. That is what §4.5's fail-safe reasoning leans on, and removing the M4 doesn't change it: still no second acoustic loop, whichever way the guitar reaches Bela's input.

**Ground rule for all development and measurement, unchanged for if/when this returns:** the amp is silent, on headphones, or in another room. The only loop that is live during testing is the controlled one. Amp volume is reintroduced as a deliberate, late-phase variable (Phase 7), never as an uncontrolled background condition — and the moment it is reintroduced, §4.5's "no hardware kill needed" reasoning must be revisited.

---

## 3. Hardware facts

### 3.1 Bela Gem Stereo

From the published spec (bela.io, learn.bela.io, shop.bela.io — see Sources). **Nothing in
this table is measured on Abel's unit yet** — Phase 0 does that, and until it does, every
number here is a starting assumption, not a fact.

| | |
|---|---|
| Base board | PocketBeagle 2 — **multi-core** |
| Audio in / out | 2 in, 2 out — up to **24-bit, 96 kHz**. Stereo line in; **differential line out + headphone out** |
| Round-trip latency | **Sub-millisecond** (spec). Measure electrically in Phase 0 and treat that number as the fact; it sets the phase-condition comb spacing (§11) |
| Audio input full scale | TBC |
| Audio input impedance / buffer need | **Unknown.** §4.1's buffered-split rule stays in force until measured |
| Analog in | 8 channels, 16-bit, ~22–24 kHz, DC-coupled. Voltage range **TBC** |
| Analog out | **None.** `analogWrite()` is not available on Gem. Not used by gen1 anyway (§2.1) |
| Digital I/O | 16 pins, 3.3 V logic |
| Connectivity | **USB-C** device port (IDE + gadget networking); USB-A host for WiFi / BT / Ethernet / MIDI; I²C + Qwiic |
| Storage | microSD (flashed card ships in the starter kit) |
| Power | 5 V; battery operation supported |
| IDE / toolchain | Browser IDE (TypeScript/Vue). C++, Pure Data, SuperCollider supported |
| Kit contents | Bela Gem Stereo board, PocketBeagle 2, flashed microSD, USB-C cable, baseplate, spacers, screws |

**Load-bearing unknowns, to close in Phase 0** by measurement or by the Bela docs for this
exact board — do not proceed past bring-up on the assumed values:

1. Audio input impedance, and whether a passive pickup needs a buffer in front (§4.1).
2. Analog-input full-scale voltage, and how a 3.3 V-fed expression pedal maps into it (§4.3).
3. ~~Whether `scripts/deploy.sh`, **pybela** and the **`Watcher`** library work as expected
   on this board and IDE, or need replacing.~~ **Resolved 2026-09-11, with caveats.**
   `build_project.sh`/`stop_running.sh` run fine directly on the board (`scripts/deploy.sh`'s
   own host-side path is still untried). Watcher/pybela streaming works end to end
   (`bela/watcher-check/` + `host/rig/watcher_check.py`), but neither is drop-in:
   - `Watcher.h`/`Watcher.cpp` are **not** part of Bela core — pybela's own docs say so —
     and had to be vendored into the project folder from the `watcher` submodule of
     `github.com/BelaPlatform/pybela` (commit 60a09e0). Two lines had to be added to make
     that 2023-era code compile against this board's Bela (`master`, not the `dev` branch
     pybela's docs call for): `#include <array>`, and `#include <Bela.h>` moved *before*
     `#include <Watcher.h>` (the tutorial's own ordering leaves `rt_fprintf` undeclared).
   - The **`pybela` package on PyPI (0.1.0) is stale** relative to its own GitHub source
     (2.0.3): it checks `ws.open`, a `websockets` API that no longer exists in current
     `websockets` releases, so every connection attempt raised `AttributeError` inside an
     asyncio task and hung with no visible error. `host/rig/watcher_check.py` carries a
     small compat shim (patches `.open` back onto the installed `websockets` class) rather
     than fighting a `pip install` from GitHub, which silently failed in this environment.
   - Needs **Python ≤3.12** on the host — `pybela`'s `jupyter-bokeh` dependency doesn't
     build on 3.13 (`pipes` module removed). `host/.venv` is 3.11.
   - Each Bela-side program run accepts **one streaming client for its lifetime**; there's
     no reconnect after a client disconnects mid-session without restarting the Bela
     program. Plan the host harness around that.
4. Line-out vs headphone-out level, and which of the two feeds the power amp → exciter chain.
5. Real round-trip latency at the block size actually used, and the resulting comb spacing.
6. CPU headroom for STFT + Goertzel bank + N biquads, and the inter-thread pattern the Bela
   Gem migration guide calls for on its multi-core base.

### 3.2 Rest of the chain (as actually wired, 2026-09-11)

| Item | Status | Notes to fill in during Phase 1 |
|---|---|---|
| Bela Gem Stereo (Starter Kit) | On hand from 2026-09-10; SSH bring-up confirmed 2026-09-11 (root@192.168.7.2, key auth, `build_project.sh` runs on-board) | Browser IDE still unchecked |
| Buffer: **M4** interface, buffered instrument input, direct out | **Removed from the signal path 2026-09-13** — was the source of a confirmed ground loop (shared USB ground with the tethering Mac, see §4.4). Still owned, not currently used | A passive DI box (ground-isolating, no shared power needed) is on order to replace it |
| Guitar → Bela: **direct, unbuffered** (2026-09-13, temporary) | In the signal path now, in place of the M4 | Deliberately contradicts §4.1's buffer rule — known cost (treble roll-off from impedance mismatch), accepted until the DI box arrives. Guitar is mono; Bela audio in is stereo — expect signal on channel 0 (left) only until confirmed by ear/meter |
| Exciter: **Dayton** surface transducer | On hand, wired straight after the power amp | Exact model, impedance, power handling, mounting position on the body |
| Power amp: **Dayton DTA120** | On hand, wired straight from Bela audio out to the exciter — **no pedal or other device in between** (§4.5) | Model confirmed; output power, gain, input sensitivity, and the fixed gain setting once chosen (ground rule 10.5) |
| Guitar volume pedal (passive) | On hand, **not currently in the signal path** | Loop gain is presently fixed by the DTA120's own gain control. Can be reintroduced later for foot control (§8), but is no longer required as a safety interlock — see §4.5 |
| Expression pedal (passive) | On hand, not yet wired (Phase 6) | Pot value and TRS wiring convention to be identified — see §4.3 |
| Guitar: **Epiphone, humbucker** | On hand, in use | Which tuning, exact model — affects the partial map |

**Block size and latency.** Bela's selling point is a hard-real-time audio thread with
configurable, very small block sizes. The exact round-trip latency of *this* rig is a
Phase 1 measurement, not an assumption — see §11.

---

## 4. Electrical constraints and gotchas

### 4.1 The audio input may need a buffer in front of a passive pickup

**Unverified on the Gem Stereo — see §3.1.** A passive guitar pickup driving a low input
impedance loses high end and level; it is a known limitation on Bela boards in general, and
Bela's own forum recommends a buffer stage to raise input impedance where it applies.

**Ground rule:** until measured, treat the feed to the board as **always buffered**. Either
use an active/buffered splitter, or take the feed from a buffered pedal output. Never wire a
bare passive pickup straight in and then spend a week debugging "the detector is missing the
high partials."

**Deliberately violated, 2026-09-13, not silently:** the M4 (which satisfied this rule) was
removed after being identified as the source of a ground loop — see §4.4. The guitar now
goes directly into Bela's audio in, unbuffered, until a passive DI box arrives. Detection
work done against this window's recordings should expect degraded high end and treat it as a
known artefact of this period, not a detector defect. A passive DI was chosen deliberately
over an active one specifically because it restores this rule *and* fixes the ground loop at
the same time (a transformer is galvanically isolating; see the DI-vs-active discussion this
decision came from — passive attenuates rather than boosts, which is fine here because the
humbucker is fairly high output and Bela's own input PGA has up to 59.5 dB of gain to spare).

### 4.2 Levels

- Audio input full scale and available input gain are **TBC** (§3.1). Whatever they turn
  out to be, gain-stage into them deliberately: aim for a healthy but headroom-preserving
  level (target ~-12 dBFS peak on normal playing, so feedback growth has room before
  clipping).
- The DSP output goes straight to the power amp (Dayton DTA120) — no pedal in between in this build (§4.5). **The power amp's gain is the loudest link in the chain.** Set it once, write it down, tape it, and never let it be the variable under test.

### 4.3 Expression pedal into an analog input

A passive expression pedal is a potentiometer on a TRS jack. Wiring: **3.3 V → pot end, wiper → analog in, GND → pot other end**.

Three things to get right:

1. **TRS convention varies by brand** (which of tip/ring is the wiper, and pot taper/value, typically 10 k–25 k). Identify yours with a multimeter before wiring, not after.
2. **Scaling.** The board's analog input full-scale voltage is **TBC** (§3.1) — do not
   hard-code 0–1 or assume any particular range. Calibrate heel and toe at startup, or store
   the measured endpoints in the rig profile and normalise in software.
3. **Protection and smoothing.** A series resistor (~10 kΩ) on the wiper into the analog pin, and a low-pass smoother in software (a one-pole around 10 Hz) — analog inputs are noisy and a jittery control on a feedback regulator is audible.

### 4.4 Grounding

Two amplifiers, one instrument and a transducer bolted to the guitar is a ground loop waiting to happen, and hum inside a feedback loop is not just hum — it is a partial the regulator will faithfully detect and fight. Plan for it: shared power strip, and be ready to add a transformer isolator on one of the two chains.

**Confirmed, not just anticipated, 2026-09-12/13.** This actually happened, though not from the two-amplifiers case above: the M4 audio interface and Bela were both drawing ground reference through the same Mac at once (M4 over USB audio, Bela over the USB-C dev tether) while also being linked by an analog cable — two paths back to one ground. Symptoms were loud, complex noise (clicking, static, irregular pulsing) audible identically on every downstream device tried (headphones, Genelec active monitors, the DTA120→exciter chain), because the noise was already in the signal at Bela's output jack. Isolated methodically: a plain electrical loopback (Bela out wired straight to Bela in, no second device) was clean regardless of code/Watcher/CPU load; a program that never reads audio input at all was clean regardless of deployment path; the M4's own output, monitored directly with the M4 never touching Bela, was clean; the M4 through Bela, on the plainest possible passthrough with zero instrumentation, was noisy. Fix: take the M4 off the Mac's USB entirely (its own wall power only) — the loud noise disappeared, leaving a much quieter garden-variety 50 Hz hum (M4's own supply vs. Bela's Mac-referenced ground, still tied together by an unbalanced cable — the standard remaining case this section already calls out). **Lesson for any future device added to this rig:** if it shares a USB host with Bela *and* has an analog connection to Bela, check for this before blaming anything else, especially before blaming code.

### 4.5 Fail-safe (revised 2026-09-11 for the rig as actually built)

The rig can produce a lot of acoustic energy on its own initiative — that has not changed.
What has changed is the shape of the risk, because of two facts about this specific build:

1. **There is no second, uncontrolled acoustic loop.** Per §2.3, the guitar goes only into
   Bela's audio in (directly as of 2026-09-13, previously via the M4); nothing feeds a live
   guitar amp or PA. The failure mode a hardware kill traditionally guards against — a
   runaway controlled loop *compounding* with an independent acoustic loop through a stage
   amp — is not wired at all right now, and removing the M4 doesn't change that.
2. **Bela audio out feeds the Dayton DTA120 power amp directly into the exciter, with no
   pedal or other device in between.**

**Ground rule (Abel, 2026-09-11):** given (1) and (2), no dedicated hardware kill switch is
required in this build. The DTA120 + exciter combination is judged safe by Abel even in a
full, unmuted runaway ("the signal explodes") — it cannot put out anything that endangers
Abel, the instrument, or the room. A foot-operable kill is therefore not a required
interlock here, though one may be reintroduced later (e.g. a volume pedal restored for
foot control of loop gain per §8) and would then resume the role.

**This does not touch what was never optional:**

- **Software kill:** a hard output ceiling in the DSP that no automatic tuning
  may raise, and a mute-on-error path (any xrun, NaN, denormal storm, or watchdog timeout ⇒
  output muted). See ground rules §10.2–10.4. The ceiling and the error path are unchanged;
  the watchdog leg of this is suspended for `gen1-multicell-live` as of 2026-09-17 — see
  §10.1's second item for what that costs and what replaces it.
- Every session starts with the DSP output ramped up from silence, never resumed at the
  previous session's level. Loop gain itself is presently whatever the DTA120's gain control
  is set to — fixed once chosen, never touched mid-session (ground rule 10.5).
- **Abel present for every real-loop run** (ground rule 10.1) still holds. Presence is no
  longer "a foot on a pedal"; it means a person is there to judge the take, and the DTA120's
  own power switch is a trivial, always-reachable manual backstop (Abel, 2026-09-11) —
  available, not a required interlock.

**This reasoning lapses the moment either fact above stops being true.** If a live guitar
amp or PA is ever introduced (§2.3, Phase 7), or anything is added between Bela out and the
power amp, re-examine whether a hardware kill is needed again before the next real-loop run.

---

## 5. The physics we are fighting

**Why feedback picks a winner.** In the controlled loop, every frequency where loop gain ≥ 1 and loop phase is a multiple of 360° will grow. Many partials satisfy that at once. What makes one win is not that the others were never candidates — it is that the fastest-growing mode reaches the loop's first nonlinearity (power amp clipping, exciter excursion limit, any limiter in the chain) first, and that nonlinearity then reduces *the whole loop's* gain. One mode saturates the shared gain, and every other mode is pushed below unity and dies.

This is homogeneous gain saturation. It is the same mechanism that makes a homogeneously-broadened laser run on a single line.

**The fix, stated as physics:** make the saturation *inhomogeneous*. Give each partial its own gain reservoir. Then partial A saturating its own cell does not steal gain from B, C, D — they keep growing until each hits its own target. Multiple modes coexist by construction.

**This is why the actuator is a per-partial regulator, not a notch.** A fixed-depth notch is a blunt instrument: too shallow and the mode still wins, too deep and you have killed the note. What we want per partial is a **compressor/limiter cell whose target is a level, not a depth** — it applies exactly as much reduction as it takes to hold that partial at the target, and no more. "Suppression" in this project always means "regulation to a target," never "removal."

A single global limiter after the cells stays in the design, but only as a safety backstop. If it is doing musical work, the cells are misconfigured.

**Corollary worth remembering for gen2:** loop delay sets which partials get positive feedback (phase = 360°·n). A small variable delay or all-pass in the loop is therefore a second, completely different way to redistribute feedback across modes — orthogonal to gain regulation. Out of scope for gen1; note it and move on.

---

## 6. Gen1 DSP architecture

```
 in ─┬─────────────────────────────────────────► [cell 1] ─► … ─► [cell N] ─► [safety limiter] ─► out
     │                                              ▲               ▲
     └─► [analysis: STFT + growth scorer]            │               │
                    │                                │               │
                    └──► [ ALLOCATOR ] ──────────────┴───────────────┘
                              ▲
         [candidate list from rig profile / Goertzel bank]
```

### 6.1 Analysis layer

- STFT on an auxiliary (non-audio-thread) task so it can never miss the audio deadline. Bela provides an `Fft` class and NE10 NEON FFT on board.
- **Long window, short hop.** Window length sets frequency resolution (adjacent string partials must be resolvable); hop sets detection latency. These are independent — do not shorten the window to get speed. Starting point: 2048-sample window, 128–256-sample hop, Hann, with **quadratic peak interpolation** for sub-bin frequency accuracy.
  - **Updated 2026-09-14: the window is 8192, the hop stays 256.** 2048 was the starting point and it was too short to obey the rule it was written under. At 44.1 kHz a 2048 bin is 21.5 Hz, which meant (a) the prominence test's guard band excluded everything below 172 Hz — the bottom three open strings' fundamentals could never be allocated a cell at all, (b) the keep-out radius between two cells was 129 Hz, so most pairs of guitar partials could not both be regulated, and (c) the prominence shoulders landed on neighbouring harmonics instead of in the valleys between them. 8192 gives 5.4 Hz bins and fixes (b) and (c). Detection latency is unchanged because it is set by the hop. The cost is paid back by only analysing a band rather than the full spectrum: measured on the board, ~40 % audio-thread CPU at N=12 versus ~38 % for the 2048 version.
  - **Corrected 2026-09-17: (a) was NOT fixed on 2026-09-14 — it was made worse.** That change claimed a 43 Hz detection floor, which was true of the window change alone; the band-limiting optimisation landed in the same commit rewrote the prominence guard to reject bins below `gAnalysisLoBin + kNeighborOffsetBins*2`, so the band's own 45 Hz floor was **added** to the 8-bin shoulder margin and the real floor became 86.1 Hz. The low E fundamental (82.41 Hz) is the only standard-tuning string fundamental below that line, so it alone could never bind a cell, was never regulated, and ran away into the output ceiling while every other string was held at the target — audible as "the low E vibrating extremely and the rest quiet". The 43 Hz floor never shipped.
  - **The rule this now follows: the detect band and the filled band are two different things.** The *detect* band is the bins that may hold a peak — it **is** the detection floor, and it equals the actuator's own frequency range (`kMinCellFreqHz`–`kMaxCellFreqHz`), because a cell must be able to sit exactly where its detector found the peak. Binding outside it parks the actuator where the partial isn't, the detector never sees its own cut work, and the cut grows to `maxCut` — a permanent notch on a frequency nothing is playing. That failure was already reachable at the *top* end before the fix: peaks up to ~2460 Hz could bind while the actuator was clamped to 2000 Hz. The *filled* band is the bins converted to dB at all — the detect band widened by one shoulder span at each end. It is an optimisation and must never set the floor. At 44.1 kHz the floor is now 53.8 Hz (50 Hz mains still excluded, drop-D at 73.4 Hz included), computed and printed at startup rather than asserted in a comment.
  - **Open, not fixed: the shoulder margin costs 8 *bins*, so its cost in Hz scales with the sample rate.** At 96 kHz with this window it is 94 Hz and the floor lands back at 105 Hz, above the low E. `rig-profile.json` still lists 44.1 / 48 / 96 kHz as an open Phase 0/1 choice, so this is reachable by changing the rate alone with no edit to the sketch. The sketch prints a warning at startup if the floor ends up above 82.41 Hz. The fix when that fires is to scale the window with the sample rate (same resolution in Hz), not to shave the margin.
- Per-bin features maintained across frames:
  - magnitude
  - **growth rate** (dB per frame, smoothed) — the important one
  - **narrowness** (peak-to-neighbouring-bin ratio, PNPR)
  - **persistence** (how many consecutive frames it has been a peak)

Literature on howling detection uses exactly this family of criteria — PAPR (peak-to-average), PNPR (peak-to-neighbouring), PHPR (peak-to-harmonic), IMSD (interframe magnitude slope deviation), and temporal sparsity measures. Note one inversion for our case: PA/PHPR-style criteria exist to tell *feedback* from *music* so feedback can be killed. Here the feedback **is** the music. We are not detecting "is this howling," we are detecting "is this partial running away."

### 6.2 Allocator

A pool of N gain cells. N = **1 for the first working version**, then 4, target 6–8.

Each cell is `FREE` or `BOUND(f, gain_reduction, envelope)`.

Rules:

- **Arm on growth, not on rank.** A cell is allocated to a frequency when its growth score crosses threshold — *not* when it becomes the loudest bin. This is what buys back the latency (see §7).
- **Glide window.** If a bound cell's peak moves within ±~50 cents between frames, the cell follows it (smooth frequency slew, no discontinuity in the biquad coefficients). Outside that window it is a different event, not the same partial moving.
- **Release.** When a bound peak's energy falls below the release threshold for a hold time, the cell ramps its gain reduction back over the release time and returns to `FREE`.
- **Anti-chatter.** Minimum hold time after allocation; a short lockout before the same frequency region can be re-allocated.
- **One cell per partial.** No two bound cells may sit within the keep-out radius (~110 cents) of each other. **Added as an explicit rule 2026-09-17**, after two pairs of cells were observed doubled on one partial live (cells 0 and 11 both on 80 Hz, cells 6 and 9 both on 734 Hz). It had been implicit in a single occupancy test at candidate-gather time, which covers binding and nothing else — the glide had no occupancy check at all, so a cell that bound to a sidelobe outside the radius then walked uphill into the peak and collided with the cell already there; and the candidate list's own entries were never mutually exclusive, so two close peaks could both bind in one frame. This matters beyond tidiness: every cell detects from the raw pre-cell input, so two cells on one partial each compute the **full** correction and both apply it — the partial is cut twice, the control loop's gain is doubled, and the partial is driven well below target. Enforcement is now at all three points where the invariant can break (bind, co-bind, glide).
- **Collision ⇒ the newcomer gives way.** When two bound cells do end up on one partial, free the one with the fewer bound frames, not the least active. Their gain reductions are near-identical by construction — they are measuring the same partial — so the least-active tiebreak below is arbitrary there and flips frame to frame. Bound-age is stable: the incumbent is regulating, the one that glided in is the intruder.
- **No cells free?** Steal the cell whose partial is least active (lowest current gain reduction), not the oldest.

### 6.3 Actuator cell

One **peaking-EQ biquad with negative gain** per cell — not a fixed notch. Parameters:

- centre frequency: from the allocator, slew-limited
- Q: fixed to start (start around Q = 8–20; narrow enough not to shade neighbouring partials, wide enough to tolerate frequency error). Q is a tuning axis, not a per-frame variable.
- gain (dB, negative): driven by a **per-cell envelope follower comparing that partial's level to a target**. Fast attack, slow release.
  - **Reduction profile, added 2026-09-17.** The reduction was a brick wall — `cut = clamp(partial − target, 0, max_cut)`, pinning the partial *at* the target, already ∞:1. It is now `cut = (1 − slope) · (partial − target)` with an optional soft knee, where `slope` is dB out per dB in above the threshold: `+1` turns the cell off, `0` is the brick wall (the default, and exactly the old law), and **negative is over-compression** — the partial is pushed *below* the target, further below the louder it gets.
  - Measured in `loop_sim.py`: negative slope recruits **no** additional modes (1/6 at every slope with the master unit at unity), so the tempting idea that cutting the winner harder hands gain back to the others — §5's shared saturation — does not survive contact with the plant. What it does is collapse the **spread** between the modes that do sustain: 13.0 dB → 6.5 dB at master +18 dB, slope −1.0. So `master_boost_db` recruits modes and `slope` equalises them; they are not substitutes. Past about −2.0 it starts costing modes by suppressing one below the level at which it can sustain at all.
  - attack: 1–5 ms. **The attack must be faster than the loop's growth rate** or the mode gets away.
  - release: 100 ms – 2 s. This is a musical parameter — it sets how long a partial stays "used up" before it can bloom again, i.e. how the texture breathes.
- On release, the gain **ramps back**; it never resets instantly, or a returning mode gets a free run.

Coefficient updates must be interpolated per-block (or per-sample-crossfaded) — jumping biquad coefficients inside a live feedback loop produces clicks that the loop then amplifies.

### 6.3a Master upward unit

Added 2026-09-14 at Abel's request. One global gain, in dB (`master_boost_db`),
applied to the input **ahead of the cells** — on the signal the STFT and every
per-cell detector read, not on the cell chain's output.

The cells only ever cut, so by themselves they cannot start a partial that is not
already sustaining — what they do is stop a winner running away, which per §5 is
what was pushing every other mode below unity. This control supplies the other
half: a mode sustains when its round-trip loop gain reaches unity, and a global
gain multiplies that loop gain for **every** mode at once. Modes sitting below
unity are lifted towards it, and modes too quiet to be detected at all rise into
candidate range; the cells then hold whichever of them take off at the target
rather than letting one saturate the loop again. Turning this up is the direct
lever on how many partials sustain.

**Its placement is load-bearing, not incidental.** Put it after the cells and
they regulate a partial to the target, and the master then multiplies what they
just regulated — which they cannot see or correct. The output leaves the target
by exactly the master setting and walks into the ceiling. Ahead of them, a bound
partial is still regulated to the target however high this goes, and the lift
survives only on partials no cell has taken, which is exactly the population it
exists to act on. Measured in `loop_sim.py` over the peaked six-mode plant —
modes sustained, and the fraction of output samples hitting the ceiling:

| master | after the cells | ahead of the cells |
|---|---|---|
| +3 dB | 3/6, 0.00 % | 3/6, 0.00 % |
| +6 dB | 3/6, 1.60 % | 4/6, 0.00 % |
| +9 dB | 4/6, 24.31 % | 5/6, 0.00 % |
| +12 dB | 4/6, 50.96 % | 5/6, 0.00 % |
| +18 dB | 5/6, 82.17 % | 6/6, 0.00 % |

Ahead of the cells is better on both counts at every setting, and stops clipping
being the price of recruiting another mode.

It is still blunt. Being broadband it changes no partial's rank relative to
another, so it lifts the noise floor along with everything else, and every dB of
it is a dB the cells must spend from `max_cut_db` to hold the winners down.
Expect the useful setting to be bounded by those two things rather than by the
slider's own range.

**Measured context for how much this has to do, 2026-09-14.** The rig's loop gain
is sharply peaked across modes and very few modes are anywhere near unity. The
open-loop ramp with no cells at all sustains 2 partials, an octave pair. The two
multicell takes of 2026-09-13 peak at 0.068 and 0.153 against a 0.5 ceiling —
nowhere near any nonlinearity — which also means §5's shared-saturation
mechanism is barely engaged in the regime being played in, so there is not much
stolen gain for the cells to hand back. The offline closed-loop simulator
(`host/harness/loop_sim.py`) shows the same thing: with six modes at
g = 1.12, 1.04, 0.85, 0.65, 0.45, 0.25, the cut-only law sustains **1** — and the
g = 1.04 mode, above unity on its own, dies purely from the dominant mode's
shared compression, which is §5 behaving exactly as described.

### 6.4 Alternative fine-tracker, worth prototyping

A **constrained pole-zero adaptive IIR notch** (the classic Regalia-style structure) locks onto a sinusoid's exact frequency with no FFT at all and glides beautifully. It handles jumps badly — it can sit stuck on the departed frequency. The natural pairing is therefore: **FFT/Goertzel as supervisor** (sets and re-seeds the centre frequency, handles jumps) + **adaptive notch as fine tracker** (handles glide with sample-accurate precision). Evaluate this in Phase 5; do not build it first.

---

## 7. The jump problem

Abel's observation, restated precisely: slow frequency drift (a partial gliding upward as it gains energy) is trivial for an FFT tracker. A **discontinuous jump** — the feedback abandoning one partial and appearing on an unrelated one — is the hard case.

**Why it is hard.**

1. A continuity-based partial tracker actively fights you: it tries to interpolate a path between the old and new frequency and ends up notching everything in between, or nothing.
2. During the changeover, the old cell is parked on a dead frequency and the new mode runs unregulated.
3. Detection latency now costs real money. If the new mode's loop gain is, say, 1.4 and the loop round-trip is ~2 ms, it grows on the order of ~10 dB per 10 ms. A 46 ms STFT frame is far too slow to catch it before it is the winner.

**Five fixes, in order of importance.**

1. **Detect by growth rate, not by rank.** This is the big one. A mode that is about to take over is *already growing exponentially* while it is still 20–30 dB below the incumbent. Scoring `d(magnitude_dB)/dt` per bin catches it there, which converts an impossible latency problem into a comfortable one. By the time a jump is audible as a jump, we have already been regulating it for tens of milliseconds.
2. **Allocate, don't track.** Model the problem as slot allocation with a glide window (§6.2), not as continuous partial tracking. Within ±50 cents ⇒ the same partial moving. Outside ⇒ a new event, a new cell. One rule handles both the easy case and the hard case, and it never invents a glide that did not happen.
3. **Enumerate the candidates in advance.** Jump destinations are *not* arbitrary. They are string partials (tuning is known) times the loop's phase condition, shaped by body and exciter resonances. Measure the exciter→body→pickup transfer function once with a swept sine (Phase 1) and you have a ranked candidate list. Then run a bank of cheap narrowband detectors — Goertzel or complex one-pole resonators, ~3 operations per sample each — on the top ~100 candidates, in parallel with the STFT. A hundred of those is nothing for PocketBeagle 2, and it gives **block-rate** detection latency instead of frame-rate. This is the direct structural answer to "jumps are a problem": we know where it can jump to, so we watch those places continuously.
4. **Asymmetric envelopes.** Attack fast enough to beat loop growth; release slow. A jump then costs at most one attack time of unregulated growth.
5. **Never fully reset a released cell.** Ramp its reduction back over the release time, so a partial that jumps away and returns does not get a fresh unregulated run each time.

**Latency budget to hold ourselves to** (validate in Phase 1, then treat as a contract):

| Stage | Target |
|---|---|
| Audio round-trip through Bela | measure; keep at the smallest workable block size |
| Analysis hop (STFT) | ≤ 6 ms |
| Goertzel candidate bank | ≤ 1 block |
| Growth-score confirmation | 2–3 frames |
| Cell attack | 1–5 ms |
| **Total, jump to regulation** | **< 25 ms** |

---

## 8. Control surface

Two feet, two orthogonal jobs. Keep them orthogonal.

| Control | Physical | Governs | Musical meaning |
|---|---|---|---|
| **Volume pedal** | Passive; **not currently wired into the loop** (2026-09-11 — see §4.5) | **Loop gain** | *How much* energy is in the feedback loop — whether it feeds back at all, and how hard. Presently fixed by the DTA120's own gain control instead |
| **Expression pedal** | Passive, into Bela analog in 0 | **Regulation depth** (per-cell target level offset) | *How that energy is distributed* — heel: raw winner-takes-all; toe: maximum flattening, dense chorus of partials |

The expression pedal's primary mapping is the **per-cell target level**, i.e. how much dominance any single partial is allowed before its cell starts pulling it back. That single axis is the most musical one and should be mapped first. Candidate secondary mappings for later, one at a time: number of active cells, cell Q, release time.

Design constraint: the mapping must be continuous and monotone with no discontinuity at either end of travel, and heel-down must be genuinely transparent (bit-identical bypass of the cells, so the pedal off = the raw sound of the rig).

---

## 9. What "good" means — measurable proxies

An automated agent cannot hear "rich and complex." It needs numbers. These proxies are the contract between the agent's optimisation and Abel's ears; Abel's ratings are the ground truth that corrects the proxies.

Computed over a rolling window of the feedback output:

| Metric | What it captures | Direction |
|---|---|---|
| **Number of simultaneous partials** above −30 dB of the loudest | the headline goal | ↑ (target ≥ 3, ideally 5+) |
| **Dominance ratio** — loudest partial / sum of the rest | winner-takes-all, directly | ↓ |
| **Spectral flatness / entropy** of the peak set | overall richness | ↑ |
| **Sustain** — feedback maintained without dying | did we over-suppress? | must stay alive |
| **Mode-switch rate** | is it churning or is it stable-and-plural? | tunable target, not simply ↑ or ↓ |
| **Time-to-regulate after a jump** | §7's contract | ↓ (< 25 ms) |
| **Peak output ceiling never exceeded** | safety | hard constraint |

**Ground rule:** the agent optimises the proxies; Abel rates recorded takes 1–5; when the rating and the proxies disagree, **the proxy is wrong and gets rewritten** — the tuning is not "wrong for having found a bad-sounding optimum." Every rated take goes in the log with the parameter set that produced it.

---

## 10. Ground rules for semi-autonomous development

**Safety**

1. Real-loop tests run **only when Abel has confirmed the rig is live and he is present**. This build has no dedicated hardware kill switch to name before running — see §4.5 for why, and for what presence means here instead.
2. Every real-loop test run is time-boxed by a watchdog in the DSP: no run exceeds N seconds without a fresh heartbeat from the host, then output mutes.
   - **Suspended for `bela/gen1-multicell-live/` only, 2026-09-17, on Abel's explicit instruction** (`kWatchdogTimeoutS = 0`; every other sketch in `bela/` keeps the 120 s box). 120 s was sized for the first real-loop smoke test — phase-plan Phase 1's "ran clean for ~13 s … watchdog armed but not tripped" — and nobody plays a guitar in under two minutes, so in a playing session it fired every single time. A safety device that trips on every normal use is not being relied on, it is being worked around.
   - **What carries the load now.** §4.5 names rules 1, 3 and 4 as the net that stands in for this build having no hardware kill switch. With 3 suspended here, that is: the output ceiling (rule 1, unchanged, unconditional, every sample, not reachable from the GUI); mute on anything non-finite (rule 4, unchanged, latching, no recovery); **rule 2, Abel present, which is now load-bearing in a way it was not before**; and the DTA120's own power switch, already recorded in §4.5 as a trivial always-reachable manual backstop.
   - **What is actually lost, stated plainly.** The time box was the only thing that would have stopped a run nobody was watching. Rule 2 already forbids unattended runs and `rig-profile.json` still records `unattended_runs_permitted: false`, so on paper it guarded a case that is already prohibited — but in practice it guarded against Abel walking away, being distracted, or losing the SSH session with the project still running. That is gone, and being careful does not bring it back; setting the constant does.
   - This is a change to a rule listed under "hard rules, no exceptions", made by the person those rules belong to. It is recorded here rather than made silently, and it is reversible in one line.
3. There is a hard output ceiling in code. **No automatic tuning process may raise it.** It changes only by an explicit human commit.
4. Any xrun, NaN, denormal storm, or lost connection ⇒ mute output, do not "try to recover."
5. The power amp gain and the exciter mounting are fixed constants of the rig profile. The agent never asks Abel to change them mid-session in order to make a test pass.

**Process**

6. **One variable at a time.** Every run is a parameter set with a name, committed to git, A/B'd against the current best.
7. Every test run writes a log record: git commit hash, rig-profile hash, full parameter set, all proxy metrics, and the path to the recorded audio. A result without its parameter set is not a result.
8. The **rig profile** (measured latency, levels, loop transfer function, partial map, pedal calibration) is a single versioned file that everything reads. If the physical rig changes — different guitar, exciter moved, amp gain touched — the profile is re-measured and the version bumped. Old results are then tagged as belonging to the old profile.
9. **Simulator first, hardware second.** Parameter searches run against the offline loop simulator (Phase 2). The real rig is for validation and for musical judgement, not for grid search.
10. The DSP source, the host harness, the rig profile and the docs live in one repo. Cursor/Claude works on branches; `main` is always a state that boots and makes sound.
11. When the agent is uncertain whether a change is a correctness fix or a taste decision, it is a taste decision and it waits for Abel.

---

## 11. Open questions — to be answered by measurement, not by assumption

- Bela round-trip audio latency at the block size we actually use, and the resulting loop phase condition. **Measure in Phase 1.**
- The exciter→body→pickup transfer function: which frequency regions have the most loop gain, and how much it changes with exciter position on the body.
- Total loop round-trip delay including the mechanical path, and therefore the spacing of the phase-condition comb.
- Actual loop gain margin: how far above unity the loop sits at typical volume-pedal positions.
- Whether the exciter meaningfully drives the strings, or mostly the top — this determines whether feedback locks to string partials or body modes, and therefore how the candidate list is built.
- CPU headroom on PocketBeagle 2 (multi-core; §3.1) for STFT + Goertzel bank + N biquads at
  the chosen block size, and the inter-thread pattern the Bela Gem migration guide requires.
- Expression pedal's pot value, taper and TRS convention.
- Whether the pre-effects split has enough level for Bela without an extra gain stage.
- Does the guitar need to be damped/muted between tests, or does the loop restart cleanly from silence?

---

## Sources

- [Bela Gem Stereo — shop.bela.io](https://shop.bela.io/products/bela-gem-stereo)
- [Bela Gem Stereo & Multi — bela.io](https://bela.io/products/bela-gem-stereo-and-multi/)
- [Migrating to Bela Gem — Bela Knowledge Base](https://learn.bela.io/get-started-guide/migrating-to-bela-gem/)
- [Using scripts — Bela Knowledge Base](https://learn.bela.io/using-bela/technical-explainers/scripts/)
- [Fft class reference — Bela docs](https://docs.bela.io/classFft.html)
- [pybela — BelaPlatform](https://github.com/BelaPlatform/pybela)
- [Robust and early howling detection based on a sparsity measure — J. Audio Speech Music Proc. (2025)](https://link.springer.com/article/10.1186/s13636-025-00399-1)
- [A multi-criteria approach to optimization of acoustic feedback detection — Applied Acoustics](https://www.sciencedirect.com/science/article/abs/pii/S0003682X21003704)
