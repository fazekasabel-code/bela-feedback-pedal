/*
 * gen1-multicell-live — Phase 5 of docs/phase-plan.md: the N-cell allocator
 * (ground-rules-and-facts.md 6.2), generalising bela/gen1-cell/'s single cell
 * (Phase 4, PASSED 2026-09-13 -- see that project's header for the actuator
 * shape and the safety net, both unchanged here) into a pool. See "THIS FILE
 * IS gen1-multicell-live" below for what's different from bela/gen1-multicell/.
 *
 *     guitar -> Bela ch0 -> [ cell 0 ] -> [ cell 1 ] -> ... -> [ cell N-1 ] -> Bela ch1 -> DTA120 -> exciter
 *                              ^              ^                    ^
 *                  in -> [ STFT, aux task ] -> [ allocator: bind / glide / release / steal ]
 *
 * Each cell is its own peaking-EQ biquad + bandpass detector + envelope
 * follower, cascaded in series on the audio path -- a small multi-band
 * parametric EQ where each band's centre frequency and gain are driven
 * independently. The gain is a CUT only -- ground-rules 6.3's "peaking-EQ
 * biquad with negative gain". Detection for every cell reads the RAW input,
 * not the output of earlier cells in the chain: what a cell regulates is decided
 * from what is actually coming in, not from what the cells ahead of it did
 * to the signal, matching sc/gen1_cell.scd's and gen1-cell's Pitch.kr(in) /
 * STFT(in) philosophy directly.
 *
 * ALLOCATOR (ground-rules 6.2), all per-frame in the STFT auxiliary task:
 *   - Each BOUND cell glides with its partial (+-kGlideCents, quadratic-
 *     interpolated -- same as gen1-cell) and releases with a
 *     ramp-back (never instant) after kReleaseHoldFrames frames below
 *     kReleaseMarginDb off its own peak, once past its anti-chatter
 *     kMinBoundHoldFrames.
 *   - FREE cells bind to the loudest remaining stable prominent peaks not
 *     already occupied (bound, or in lockout after a recent release/steal).
 *   - No cells free and a candidate is still left over: STEAL the eligible
 *     BOUND cell with the smallest current cut (ground-rules 6.2: "the
 *     partial whose cell is least active", i.e. needing the least
 *     suppression right now, not the one that has been bound longest).
 *     A stolen cell's frequency and gain both glide to the new partial via
 *     the same per-sample slew/envelope machinery an ordinary glide uses --
 *     no special-cased crossfade needed, ground-rules 6.3's "coefficient
 *     updates must be interpolated" falls out of that for free.
 *
 * SAME DELIBERATE DIVERGENCES FROM THE OLD SC PATCH AS gen1-cell, carried
 * forward here for the same reasons (see that file's header for the fuller
 * argument, not repeated in full):
 *   - Binding is by loudest-stable-peak RANK, not growth rate. Ground-rules
 *     6.2 calls for arming on growth specifically so a new mode can be
 *     caught before it's audible; that is not implemented here. The reason
 *     is the same one gen1-cell's header documents: phase-plan.md logs
 *     bela/detector-passthrough's growth-arm threshold as miscalibrated for
 *     this rig's real (~1-2.5 dB/s) growth, and gating binding on it would
 *     likely mean cells rarely bind to the ordinary case at all. Jump
 *     handling (phase-plan.md Phase 5's other explicit goal) is therefore
 *     tested here only in the "does steal-least-active correctly redirect a
 *     cell once a new partial is already loud enough to be a stable
 *     prominent peak" sense, not in the "catch it while it is still 20-30 dB
 *     below the incumbent" sense ground-rules 7 describes. Retuning that
 *     threshold and wiring growth-based pre-emption in stays open Phase 5
 *     work, not done in this file.
 *   - One envelope-follower stage per cell, not two cascaded ones.
 *
 * CPU LOAD (phase-plan.md Phase 5: "watch CPU load as N grows"): Bela's own
 * audio-thread CPU monitoring (Bela_cpuMonitoringInit/Get) is wired in and
 * exposed over Watcher, rather than left unmeasured the way Phase 3 left it.
 *
 * SAFETY: kOutputCeiling is a safety constant in the CLAUDE.md sense --
 * unchanged in meaning and value from gen1-cell / gen1-passthrough /
 * detector-passthrough. No automatic tuning process may raise it (rule 1).
 * kWatchdogTimeoutS is DISABLED in this project as of 2026-09-17 on Abel's
 * explicit instruction -- see that constant for the reasoning and for what
 * carries the load instead. Everything under "cell tuning constants" and
 * "allocator constants" below is a musical/DSP parameter, not a safety
 * constant.
 *
 * RUN HISTORY: see bela/gen1-multicell/render.cpp's own header and
 * phase-plan.md's Status block for the real-loop takes, the sweep-kick A/B,
 * and the sandbox findings that led to the N=12 bump and the rebind-duck fix
 * below -- not repeated here, this file didn't exist yet when any of that
 * happened.
 *
 * THIS FILE IS gen1-multicell-live -- a sibling of bela/gen1-multicell/, same
 * N-cell allocator/actuator, same N=12, same rebind-duck safety fix, same
 * `bypass` toggle, MINUS the sweep-kick. gen1-multicell's sweep-kick exists
 * to seed feedback without a human playing, for repeatable A/B testing --
 * exactly the opposite of what a live-playing session wants, so it's not in
 * this file at all rather than merely disabled. Built 2026-09-13 alongside
 * the N=4->12 bump and the rebind-duck fix, so Abel could feel those changes
 * live, not just read metrics from a recording afterwards.
 *
 * 2026-09-14 CHANGE SET ("only 1-2 partials ever sustain; make it hold 6-8"),
 * each item argued at its own constant below rather than here:
 *   1. STFT window 2048 -> 8192, hop unchanged (kFftWindow). At 21.5 Hz/bin the
 *      detector could not see anything below 172 Hz -- the bottom three open
 *      strings -- and its keep-out radius forbade two cells within 129 Hz of
 *      each other, which is most of the guitar. Paid for by analysing only the
 *      band the cells actually use, so it costs less per hop, not more. (That
 *      optimisation also silently raised the detection floor to 86 Hz and hid the
 *      low E -- fixed 2026-09-17, see the DETECT/FILLED band comment.)
 *   2. Glide and keep-out radii became cents-based (kGlideCents), which is what
 *      ground-rules 6.2 specifies and a fixed bin count could not express.
 *   3. The release high-water mark now decays (kPeakDecayDbPerSecond). Latched,
 *      it made every cell release a few frames after each pluck's transient.
 *   4. Prominence threshold is live and starts at 10 dB, not a fixed 25 dB
 *      (kProminenceDbStart). 25 dB was calibrated for a runaway detector, whose
 *      job is to fire rarely; measured over the recorded takes it is met by
 *      exactly one peak per take, which is most of why one cell ever bound.
 *   5. Steal is no longer unconditional when the pool is full (kStealMarginDb).
 *      It was costing a steal every 30-50 ms -- cells ripped off partials for
 *      challengers that were not even louder.
 *   6. The release-side anti-chatter constants are live, at their old values
 *      (kMinBoundHoldFrames) -- NOT retuned, see that comment.
 *   7. A master upward unit (kMasterBoostDb), Abel's request: one global gain in
 *      dB, applied AHEAD of the cells, to lift the whole loop so modes too quiet
 *      to be detected at all come up into range. This is the only thing here that
 *      can raise how much energy the loop carries; everything above it decides
 *      what the cells can SEE and how steadily they hold it. Its placement ahead
 *      of the cells rather than after them is measured, not incidental -- see
 *      that constant.
 *
 * 2026-09-17 DETECTOR FLOOR FIX ("usually the low E vibrating extremely and the
 * rest are quiet"). Item 1 above raised the detection floor to 86.1 Hz instead of
 * the 43 Hz it claims, putting the low E fundamental (82.41 Hz) -- the only
 * standard-tuning string fundamental below that line -- permanently outside the
 * detector. It could never bind a cell, was never regulated, and ran away into
 * the output ceiling while every other string was held at the target. The detect
 * band is now its own thing, equal to the actuator's frequency range, with the
 * filled band derived from it; see the DETECT/FILLED band comment for the full
 * account and for the same bug at the top end. Floor is now 53.8 Hz at 44.1 kHz,
 * printed at startup rather than asserted in a comment.
 *
 * The safety net is untouched by all of it: kOutputCeiling, the non-finite mute
 * and the fade-in are exactly as they were (rules 1, 4). The watchdog is a
 * separate, later, deliberate change -- see kWatchdogTimeoutS.
 *
 * THE GUI: Bela's own Watcher/Gui plumbing (`getGui().setup()` below) is
 * already wired into every project in this repo -- opening this project in
 * the browser IDE's own GUI view plots every Watcher variable declared here
 * live, no extra code needed. That's the "simple GUI": per-cell bound/freq/
 * cut/partial-level (x12), aggregate bind/release/steal counts, in/out peak,
 * muted, and audio-thread CPU%. If the IDE's default view is too busy with
 * all of that at once, most Bela GUI views let you toggle which variables
 * are plotted -- worth trying before trimming any of this in code.
 */

#include <algorithm>
#include <array>
#include <Bela.h>
#include <Watcher.h>
#include <libraries/Fft/Fft.h>
#include <libraries/AudioFile/AudioFile.h>
#include <libraries/Biquad/Biquad.h>
#include <libraries/EnvelopeDetector/EnvelopeDetector.h>
#include <libraries/Scope/Scope.h>
#include <cmath>
#include <string>
#include <vector>

// Phase 5 (phase-plan.md: "Cell pool N = 4, then 6-8") -- bumped straight to 12,
// 2026-09-13, Abel's request, past the plan's own 6-8 ceiling. CPU load checked
// live at this N before trusting it (see setup()'s periodic rt_printf and the
// commit message) rather than assumed safe just because it compiles.
static const int kNumCells = 12;

// Live browser view for this project, 2026-09-14: Watcher's own GUI integration
// is blank on this board (see host/rig/multicell_monitor.py's header -- Watcher's
// `watcher` library only supports Bela's `dev` branch, this board is on `master`).
// Scope is a DIFFERENT thing: a core Bela feature (same family as the plain `Gui`
// class a stock Bela example was confirmed working with in the same browser),
// not part of the Watcher add-on, so it isn't subject to that same branch
// mismatch. Reachable at http://<board>/scope regardless of which project name
// is in the URL. Deliberately few channels -- Scope is a live oscilloscope-style
// trend view, not a 50-variable dashboard; multicell_monitor.py's terminal output
// is still the place for exact per-cell numbers.
static const int kScopeNumChannels = 4;   // in level, out level, cells bound, cpu%

Watcher<float>        gWatchInPeak("in_peak");
Watcher<float>        gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");
Watcher<unsigned int> gWatchCellsBoundCount("cells_bound_count");
Watcher<unsigned int> gWatchBindEvents("bind_events_total");
Watcher<unsigned int> gWatchReleaseEvents("release_events_total");
Watcher<unsigned int> gWatchStealEvents("steal_events_total");
Watcher<float>        gWatchCpuPercent("audio_thread_cpu_percent");
Watcher<unsigned int> gWatchBypass("bypass");        // host-settable, see header note

// Live tuning controls, 2026-09-14 -- see the "tuning controls" constants block
// below for ranges/safety clamps and the reasoning behind picking these five.
Watcher<float> gWatchTargetDb("target_db");
Watcher<float> gWatchMaxCutDb("max_cut_db");
Watcher<float> gWatchReleaseMs("release_ms");
Watcher<float> gWatchCellQ("cell_q");
Watcher<float> gWatchLoopGain("loop_gain");

// The 2026-09-14 "why do only 1-2 partials ever sustain" controls -- see the
// MASTER UPWARD UNIT block and kProminenceDbStart below.
Watcher<float> gWatchMasterBoostDb("master_boost_db");

// Envelope attack, made live 2026-09-17 -- see kAttackMs.
Watcher<float> gWatchAttackMs("attack_ms");

// Reduction profile, 2026-09-17 -- see the REDUCTION PROFILE block below.
Watcher<float> gWatchSlope("reduction_slope");
Watcher<float> gWatchKneeDb("knee_db");
Watcher<float> gWatchProminenceDb("prominence_db");

// Release-side anti-chatter, made live 2026-09-14 -- see kMinBoundHoldFrames.
// Their defaults are exactly the values that were compile-time constants before,
// so exposing them changes no behaviour on its own.
Watcher<float> gWatchMinHoldMs("min_hold_ms");
Watcher<float> gWatchReleaseMarginDb("release_margin_db");

// Per-cell telemetry -- named at runtime in setup() (cell0_bound,
// cell1_bound, ...). Constructed via reserve()+emplace_back exactly once, so
// the vector never reallocates after setup(): Watcher registers `this` with
// the WatcherManager at construction time, so any later reallocation moving
// the objects would leave the manager holding stale pointers.
static std::vector<Watcher<unsigned int>> gWatchCellBound;
static std::vector<Watcher<float>>        gWatchCellFreqHz;
static std::vector<Watcher<float>>        gWatchCellPartialDb;
static std::vector<Watcher<float>>        gWatchCellCutDb;

// ---------------------------------------------------------------- constants

// Safety constants -- see header comment. kOutputCeiling and kFadeInS are
// identical in meaning and value to gen1-cell / gen1-passthrough /
// detector-passthrough. kWatchdogTimeoutS is NOT -- see below.
static const float kOutputCeiling = 0.5f;

// WATCHDOG DISABLED IN THIS PROJECT, 2026-09-17, Abel's explicit instruction.
// 0 = no time box. Any positive value re-arms it, and the mechanism below is
// untouched, so re-enabling is a one-line change to this constant.
//
// This is a change to a named hard rule, not a tuning tweak, so the reasoning is
// recorded here as well as in CLAUDE.md rule 3 and ground-rules 10.1.
//
// WHY. 120 s was sized for a real-loop SMOKE TEST -- the first time the exciter
// was driven at all (phase-plan.md's Phase 1 entry: "ran clean for ~13 s ...
// watchdog armed but not tripped"). It is the wrong instrument for a playing
// session: nobody plays a guitar in under two minutes, so the time box stopped
// being a backstop and became the thing that ends the take. A safety device that
// fires on every single normal use is not being relied on, it is being worked
// around, and that is worse than not having it.
//
// WHAT IS NOW CARRYING THE LOAD. CLAUDE.md rule 6 names rules 1, 3 and 4 as the
// safety net that stands in for this build having no hardware kill switch. With
// 3 gone for this project, that is:
//   - rule 1, the output ceiling: kOutputCeiling, unchanged, unconditional, on
//     every sample, not reachable from the GUI.
//   - rule 4, mute on anything non-finite: unchanged, latching, no recovery.
//   - rule 2, Abel present for every real-loop run: unchanged, and now doing
//     strictly more work than it was. This is the one that replaces the time box.
//   - the DTA120's own power switch, which ground-rules 4.5 already records as a
//     trivial, always-reachable manual backstop (Abel, 2026-09-11).
//
// WHAT IS ACTUALLY LOST. The time box was the only thing that would have stopped
// a run nobody was watching. Rule 2 already forbids unattended runs on this rig
// and rig-profile.json still records unattended_runs_permitted: false, so in
// principle it protected against a case that is already prohibited -- but in
// practice it protected against Abel walking away, getting distracted, or losing
// the SSH session with the project still running. That protection is gone. It
// does not come back by being careful; it comes back by setting this constant.
//
// SCOPE. Only this project. Every other sketch in bela/ still carries the 120 s
// box, deliberately: they are test harnesses driven by timed runs, where the box
// costs nothing and the runs are short by construction.
static const float kWatchdogTimeoutS = 0.0f;
static const float kFadeInS = 0.2f;

static const unsigned int kGuitarInputChannel = 0;

// `bypass` (host-settable via Watcher, default 0/engaged; not a safety
// constant): when 1, the N cells still run in full -- STFT, allocator,
// actuators, all of it -- but their output is not what reaches the exciter;
// the raw input goes straight through instead, exactly mirroring
// sc/gen1_cell.scd's own Select.ar(bypass, [wet, in]). Lets a live session
// A/B "does this sound different with the cells on" without redeploying.

// ---- analysis (STFT, reused from bela/gen1-cell -- see that project's
// header for the fuller rationale, unchanged here).
// 2026-09-14: window 2048 -> 8192, hop UNCHANGED at 256.
//
// ground-rules 6.1 states the rule this now actually obeys: "Long window, short
// hop. Window length sets frequency resolution (adjacent string partials must be
// resolvable); hop sets detection latency. These are independent -- do not
// shorten the window to get speed." At 2048 / 44.1 kHz a bin is 21.5 Hz wide,
// and three things followed from that, each of which on its own caps how many
// partials this can ever hold:
//   - isProminentPeak() rejects every bin below kNeighborOffsetBins*2 = 8, i.e.
//     everything under 172 Hz. The low E (82 Hz), A (110 Hz) and D (147 Hz)
//     fundamentals could not bind a cell AT ALL.
//   - kExclusionBinRadius = 6 meant a 129 Hz keep-out zone around every bound
//     cell, so two partials closer together than that structurally could not
//     both be allocated -- which is most of the guitar's useful register.
//   - the prominence shoulders at +-4 and +-8 bins (86 / 172 Hz) landed on
//     NEIGHBOURING HARMONICS rather than in the valleys between them, so a
//     genuine partial measured as barely prominent at all.
// At 8192 a bin is 5.4 Hz: the detection floor drops to 43 Hz, the keep-out zone
// to a musically sane width, and the shoulders land in the valleys. Detection
// latency is unchanged -- that is the hop, not the window.
//
// This is not more expensive than what it replaces, because of kAnalysis*Hz
// below: only the ~430 bins covering the cells' own frequency range are turned
// into dB and peak-tested, instead of all 1025 bins of the old window.
static const int   kFftWindow = 8192;
static const int   kHop = 256;
static const int   kNumBins = kFftWindow / 2 + 1;
static const int   kRingSize = kFftWindow * 2;

// ---- the DETECT band and the FILLED band, 2026-09-17. These are two different
// things and conflating them is what produced the bug this change fixes.
//
// THE BUG. Commit 220d2ae ("make the detector see the guitar") changed two things
// at once: the window 2048 -> 8192, and a new band-limiting optimisation that
// stopped converting the whole spectrum to dB. Its header claims "the detection
// floor drops to 43 Hz", which was true of the window change ALONE -- at 8192 a
// bin is 5.4 Hz and isProminentPeak()'s shoulder guard rejected bins below
// kNeighborOffsetBins*2 = 8, i.e. 43.1 Hz. The same commit then rewrote that
// guard to reject bins below gAnalysisLoBin + kNeighborOffsetBins*2, so the
// 45 Hz floor of the new optimisation was ADDED to the 8-bin shoulder margin:
//
//     floor = bin 8 (45 Hz) + 8 = bin 16 = 86.1 Hz
//
// The low E fundamental is 82.41 Hz = bin 15.3. It is the ONLY string fundamental
// in standard tuning below that floor -- A is bin 20.4, D bin 27.3, G bin 36.4.
// So the low E could never bind a cell, was never regulated, and ran away into
// clampToCeiling() while every other string was held at the target. The glide
// search was clamped to the same floor, so a cell could not even drift onto it.
// The 43 Hz floor never actually shipped; the two halves of one commit cancelled.
//
// THE FIX is to stop deriving one band from the other by arithmetic in a comment.
// There are two bands and the code now names both:
//
//   DETECT band [gDetectLoBin, gDetectHiBin] -- bins that may hold a peak. This
//     IS the detector floor. It equals the actuator's own frequency range: a cell
//     must be able to sit exactly where its detector found the peak. Bind outside
//     it and freqNow's clamp parks the actuator somewhere the partial isn't, the
//     detector never sees its own cut work, and the cut grows to maxCut -- a
//     permanent notch on a frequency nothing is playing. That failure was already
//     reachable at the TOP end before this change: peaks up to ~2460 Hz could
//     bind while kMaxCellFreqHz clamped the actuator to 2000 Hz.
//   FILLED band [gAnalysisLoBin, gAnalysisHiBin] -- bins converted to dB at all.
//     The detect band widened by a full shoulder span (kNeighborOffsetBins*2) at
//     each end, so every tested bin's taps land on real data. This is what pays
//     for the 8192 window; it is an optimisation and must never set the floor.
//
// setup() derives both and then re-clamps the detect band to what the filled band
// can actually support, so the invariant holds by construction at any window size
// or sample rate rather than by someone redoing this arithmetic. It also prints
// the resulting floor in Hz, because a comment asserting a number the code does
// not produce is exactly how this bug survived.
static const float kGrowthSmoothing = 0.6f;      // telemetry only, see header note
static const int   kStableHoldFrames = 2;
static const float kReleaseMarginDb = 10.0f;    // starting value; live, see gWatchReleaseMarginDb
static const int   kReleaseHoldFrames = 8;
static const float kSilenceDbfs = -80.0f;
// Prominence: was a fixed 25 dB, now a live control (gWatchProminenceDb) with
// this as its STARTING value. 25 dB came from bela/detector-passthrough, where
// the job was "do not false-arm on room noise" -- i.e. a threshold tuned for a
// RUNAWAY DETECTOR, whose whole purpose is to fire rarely. This project's cells
// now have the opposite job as well: finding quiet partials that are not
// sustaining yet, so they can be driven up. For that, 25 dB is backwards -- a
// partial 25 dB above its own spectral shoulders is already a winner and does
// not need help. 12 dB is the new starting point; the slider exists because the
// right value is a property of this rig's noise floor.
//
// Measured 2026-09-14 against the five recorded takes in logs/2026-09-12..13, at
// the OLD 2048 window, reproducing this exact formula. Of the twenty loudest
// distinct spectral locations in a whole take, the number clearing each bar:
//
//     threshold   first-cell  first-multi  second-multi  sweepkick  ramp-v3
//       25 dB          1           2            1            0         3
//       15 dB          4           5            3            3         4
//       10 dB          9          10           12            3        15
//        6 dB         18          17           18            3        19
//
// So 25 dB is not "generously passed by the winner and narrowly missed by the
// rest" -- it is met by ONE peak, essentially always. That is the whole of the
// observed one-bound-cell behaviour. 6 dB is too loose: across every local
// maximum in every frame (the population a live detector actually faces, which
// is dominated by noise-floor ripple with a median prominence of ~1.3 dB) 6 dB
// passes ~14 %, against ~3 % at 10 dB. 10 dB it is.
//
// Caveat worth keeping in mind when reading that table: it was measured at the
// 2048 window this file no longer uses. At 8192 the shoulder taps sit at +-21.5
// and +-43 Hz instead of +-86 and +-172 Hz, i.e. in the valleys between guitar
// harmonics rather than on top of neighbouring ones, so a genuine partial should
// measure MORE prominent than the table says, not less. That makes 10 dB a
// conservative starting point rather than an aggressive one -- but it is a
// starting point, which is why the slider exists.
static const float kProminenceDbStart = 10.0f;
static const int   kNeighborOffsetBins = 4;
// Glide and exclusion are now CENTS-based, not a fixed bin count, 2026-09-14.
// ground-rules 6.2 specifies the glide window in cents ("+-~50 cents"); gen1-cell
// approximated it as a fixed +-2 bins because at a 21.5 Hz bin that was roughly
// right in the middle of the range. It is not roughly right anywhere else: +-2
// bins is +-430 cents at 100 Hz and +-9 cents at 2 kHz. The same applies to the
// keep-out radius, which decides whether two partials may each hold a cell --
// the single most direct limit on how many partials can be regulated at once.
// A fixed bin count could not express either rule; a cents radius can, and it
// falls out of the bigger window above for free.
static const float kGlideCents = 50.0f;          // ground-rules 6.2's glide window, as written
// 2026-09-17, Abel: 110 -> 50, i.e. the keep-out radius is now exactly the glide
// window. 110 was wrong against its own stated intent and against the doc.
//
// Its comment claimed "just over a semitone ... adjacent semitones can each hold
// their own". A semitone is 100 cents and 110 was the keep-out, so adjacent
// semitones were precisely what it blocked -- the one case it was written to
// permit.
//
// And ground-rules 6.2 / 7 already define this boundary, in the glide window:
// "within +-50 cents => the same partial moving. Outside => a new event, a new
// cell." Two different radii for one question left a band, 50-110 cents, that
// could neither be glided onto (beyond glide reach) nor allocated a cell of its
// own (inside the keep-out). Measured across the range: 83-110 cents at 330 Hz,
// 55-108 cents at 1 kHz and at 2 kHz. A partial landing in it beside a live
// incumbent was unregulated for as long as that incumbent held its slot, with
// clampToCeiling the only thing underneath it.
//
// One radius, one meaning: if the glide can reach it, it is the same partial and
// one cell owns it; if it cannot, it is a different partial and may have a cell.
//
// What made this safe to shrink is the collision-resolution pass added earlier
// today. The extra width in 110 was doing a second job -- keeping cells from
// converging on one partial -- badly, since the glide ignored it entirely. That
// job now has its own explicit enforcement, so this radius only has to answer the
// question it is named for.
//
// WHAT IS LEFT, stated rather than papered over: above ~600 Hz the two radii come
// out to the same bin count and the dead zone is gone entirely. Below that the
// BIN-COUNT floors still differ -- kGlideBinRadiusMin 2 against
// kExclusionBinRadiusMin 3 -- leaving exactly one bin of it (offset 3: 83 cents
// at 330 Hz, widening to 316 cents at the low E, where a bin is a large fraction
// of a semitone). That residue is deliberate. An 8192-point Hann window's
// mainlobe is about 4 bins wide, so two cells 2 bins apart at the bottom of the
// range would both be sitting on the same peak's mainlobe. The exclusion floor of
// 3 is what stops that, and it is a property of the window, not of the
// musical rule this constant expresses.
static const float kExclusionCents = 50.0f;
static const int   kGlideBinRadiusMin = 2;
static const int   kExclusionBinRadiusMin = 3;
// Anti-chatter minimum hold, ~232 ms at hop 256 -- guards both release and being
// stolen. Now a live control (gWatchMinHoldMs), starting at this value.
//
// WHY IT IS A KNOB AND NOT A NEW NUMBER I PICKED, 2026-09-14. Replayed against
// the recorded takes, cells release at the EARLIEST MOMENT THIS CODE ALLOWS,
// over and over: mean bound episode 280 ms on detector_check and on both
// multicell takes, against a floor of kMinBoundHoldFrames + kReleaseHoldFrames =
// 48 frames = 278.6 ms. Between 50 % and 81 % of all binds are a slot returning
// to a frequency region it has already held, and that is just as true on takes
// where no steal ever happens -- so this is release/rebind cycling, a separate
// mechanism from the steal thrashing kStealMarginDb fixes, and a bigger one.
// A cell that lets go of its partial every ~280 ms is not regulating it, it is
// flickering on and off it.
//
// I have NOT picked new values, because ground-rules 6.3 calls release timing a
// musical parameter -- "it sets how long a partial stays 'used up' before it can
// bloom again, i.e. how the texture breathes" -- and CLAUDE.md rule 15 says a
// call like that is Abel's, not mine. So both constants that decide it are live
// instead, defaulted to exactly what they were.
//
// MEASURED STARTING POINT for when Abel does judge it by ear (swept offline over
// the recorded takes, 232/400/700/1200/2000 ms against 6/10/15/20/30 dB):
//   min_hold_ms = 700. Revisit fraction falls on every take -- detector_check
//     0.81 -> 0.64, first-cell-take 0.52 -> 0.35 -- at no cost at all to
//     bound-cell count, which if anything rises slightly. No cost appeared
//     anywhere up to 2000 ms. The price is real though: it roughly triples how
//     long a cell may sit on a genuinely dead partial before it is allowed to let
//     go, which is exactly the "used up" feel that section calls musical.
//   release_margin_db = 10, i.e. leave it. This one is inert. It saturates above
//     ~15 dB, never changes bound-cell count on any take, and on detector_check
//     and both multicell takes the results are bit-for-bit identical at 10, 15,
//     20 and 30 dB -- the relative test never fires there at all, only the
//     absolute kReleaseFloorDbfs and the anti-chatter timer do.
//
// Caveat on all of that: the takes were recorded before the detector changes
// above, so a partial that no cell could see then may behave differently now.
static const int   kMinBoundHoldFrames = 40;
static const float kMinHoldMsMin = 50.0f, kMinHoldMsMax = 3000.0f;
static const float kReleaseMarginDbMin = 3.0f, kReleaseMarginDbMax = 40.0f;
// 2026-09-14: gBoundPeakMagDb used to be a running max that never came down, and
// release fires kReleaseMarginDb below it. A plucked note's attack transient sits
// 20-30 dB above the level it sustains at, so the high-water mark latched onto the
// transient and EVERY cell then released a few frames later, as the note settled
// into exactly the sustain we want it to hold -- followed by kLockoutFrames during
// which it could not rebind. That alone kept the bound count near zero through the
// most useful part of every note. The mark now decays, so "10 dB below its peak"
// means below its RECENT peak.
static const float kPeakDecayDbPerSecond = 10.0f;
// Absolute backstop for release: however the relative test is doing, a partial that
// has actually gone quiet should free its cell. Sits above kSilenceDbfs so a cell is
// not held on by pure noise.
static const float kReleaseFloorDbfs = -72.0f;
// How much louder a challenger must be than the incumbent it would displace
// before a steal is allowed at all -- see the steal block in analyseFrame() for
// the measured thrashing this exists to stop. 0 reproduces the old unconditional
// behaviour, matching multicell_sandbox.py's STEAL_MARGIN_DB.
static const float kStealMarginDb = 6.0f;
// Two cells count as equally inactive if their cuts are within this of each
// other, 2026-09-17. "Least active" was a strict minimum over gCellCutDbLast,
// which is fine when the cuts differ but useless when several cells sit at
// exactly 0.0 dB -- a normal state, since a cell bound to a partial below the
// target cuts nothing. The strict `<` then made the winner whichever of them had
// the lowest INDEX, which is arbitrary: it may pick a cell sitting on a partial
// near the target over one sitting on a much quieter partial. That matters twice
// over, because the steal margin is measured against whichever cell is picked --
// so an arbitrary choice also sets an arbitrarily high bar for the challenger.
// Within the tie band, prefer the quietest partial: it is the least valuable slot
// AND the lowest bar. See the two-pass selection in analyseFrame().
static const float kStealCutTieDb = 0.5f;
static const int   kLockoutFrames = 20;          // ground-rules 6.2's "short lockout
                                                  // before the same frequency region can
                                                  // be re-allocated", ~116 ms.

// ---- cell / actuator tuning (ground-rules 6.3; same defaults as gen1-cell
// originally carried over untouched). As of 2026-09-14 the four musically-
// relevant ones (target/maxCut/release/Q) are live-tunable from the browser
// GUI while a session runs -- these are now their STARTING values, not fixed
// constants; see gWatchTargetDb etc. above and the per-sample read below.
// kFreqLagS stays a fixed constant, not exposed as a live control on purpose:
// it is what keeps coefficient updates click-free (ground-rules 6.3) rather than
// something that reads as "tone".
//
// kAttackMs WAS in that sentence too, on the grounds that attack is a safety
// margin -- ground-rules 6.3's "must be faster than the loop's growth rate" --
// and not a taste knob. As of 2026-09-17 it is live, at its same default. The
// reclassification, Abel's call, with the arithmetic that justifies it:
//
// Overshoot from a slow attack is roughly growth rate x attack time, and this
// rig's measured growth is ~1-2.5 dB/s (bela/detector-passthrough):
//
//       attack     overshoot at 2.5 dB/s
//        3 ms          0.008 dB
//      100 ms          0.25  dB
//      300 ms          0.75  dB
//     1000 ms          2.5   dB
//
// So the old 3 ms was roughly 300x faster than the requirement it was set by. The
// margin is real but it was being paid for at an absurd exchange rate.
//
// What it costs, and why Abel raised it: a pluck sits 20-30 dB above the level
// the note sustains at, so at 3 ms the envelope tracks the TRANSIENT and the cut
// slams to (transient - target) inside the pluck. That flattens the attack of
// every note, and a 20-30 dB gain change in 3 ms is itself an audible artifact --
// a second click source, separate from the rebind duck fixed the same day.
//
// A slow attack is not a compromise against safety here, it is the correct
// DISCRIMINATOR: feedback growth is slow and pluck transients are fast, so a slow
// attack ignores plucks and regulates only sustained growth, which is exactly the
// job. ground-rules 6.3 updated in the same commit rather than contradicted.
//
// Interaction worth knowing before turning it far: release is 400 ms. Past about
// 200 ms of attack the envelope is near-symmetric and the cut starts following
// each note's own amplitude envelope -- pumping. Raise release alongside it.
//
// The upper bound is 500 ms, which is 1.25 dB of overshoot at the measured growth
// rate. That is the safety margin, now stated as a number instead of a habit.
static const float kTargetDb = -24.0f;
static const float kCellQ = 10.0f;
// 300 ms as of 2026-09-17, Abel's judgement after a live session: "let's now save
// attack at 300ms that seems to be best." Was 3.0f, which is what this had
// inherited from gen1-cell. CLAUDE.md rule 16 -- the metrics are a stand-in for
// his ears, and when the two disagree the metric is the bug -- so this is the
// value, not a proposal.
//
// Costs 0.75 dB of overshoot at the rig's measured ~2.5 dB/s growth (see the
// table above), against a ceiling 27 dB above the levels this rig actually runs
// at. The safety margin is the BOUND, kAttackMsMax, not this number.
//
// NOTE the deliberate near-symmetry with kReleaseMs (400 ms): the comment above
// warns that past ~200 ms the envelope stops being asymmetric and the cut starts
// following each note's own amplitude envelope. That is now the shipped default
// and it was chosen by ear anyway. If a future session finds the cut pumping with
// the playing rather than with the feedback, raising kReleaseMs is the first move,
// not lowering this.
static const float kAttackMs = 300.0f;
static const float kReleaseMs = 400.0f;
static const float kMaxCutDb = 30.0f;
// ------------------------------------------------ REDUCTION PROFILE
// 2026-09-17, Abel: "controlling the reduction profile, probably tuning the
// ratio much more aggressive".
//
// There was no ratio to tune. The law was cut = clamp(partialDb - targetDb, 0,
// maxCut), i.e. a brick wall that pins the partial exactly AT the target --
// already infinity:1, already the most aggressive setting a normal compressor
// has. So the knob is added here in the one form that covers the whole useful
// range monotonically, including the region past the brick wall:
//
//     cut = (1 - slope) * (partialDb - targetDb),  clamped to [0, maxCut]
//
// `slope` is dB of OUTPUT change per dB of INPUT change above the threshold
// (which is the target):
//
//     slope = +1.0   ratio 1:1    -- no reduction at all, the cell is off
//     slope =  0.0   ratio inf:1  -- brick wall, pinned AT the target. This is
//                                    exactly the law this file had before, so
//                                    0.0 changes nothing and is the default.
//     slope <  0     OVER-compression -- the partial is pushed BELOW the target,
//                                    and further below the louder it gets.
//
// WHAT IT ACTUALLY DOES, measured in host/harness/loop_sim.py over the peaked
// six-mode plant, as sustained modes and the dB spread between the loudest and
// quietest sustained one:
//
//     slope        master +0 dB     master +9 dB     master +18 dB
//      0.00        1/6   n/a        5/6   7.9 dB     6/6  13.0 dB
//     -0.50        1/6   n/a        5/6   5.3 dB     6/6   8.7 dB
//     -1.00        1/6   n/a        5/6   4.0 dB     6/6   6.5 dB
//     -2.00        1/6   n/a        5/6   2.6 dB     5/6   2.6 dB
//
// Read that carefully, because it refutes the obvious hypothesis. Over-
// compression recruits NO additional modes -- at master +0 it is 1/6 at every
// slope. The idea that cutting the winner harder hands gain back to the others
// (ground-rules 5's shared saturation) does not survive contact with this
// plant, and the real-rig measurements say the same thing: the 2026-09-13 takes
// peak at 0.068 and 0.153 against a 0.5 ceiling, nowhere near the nonlinearity
// that mechanism needs.
//
// What slope does instead is collapse the SPREAD -- 13 dB down to 6.5 dB at
// master +18. That is the difference between one pitch with five whispers under
// it and six pitches audible as a chord, which is the actual goal. So the
// division of labour is: kMasterBoostDb recruits modes, slope equalises them.
// Past about -2.0 it starts costing modes (6/6 -> 5/6), by suppressing one below
// the level at which it can sustain at all.
static const float kSlope = 0.0f;         // 0.0 == the pre-2026-09-17 brick wall
static const float kSlopeMin = -3.0f, kSlopeMax = 1.0f;
// Soft-knee width in dB, centred on the threshold. 0 is a hard knee, which is
// what this file did before, so it is the default.
static const float kKneeDb = 0.0f;
static const float kKneeDbMin = 0.0f, kKneeDbMax = 24.0f;

static const float kFreqLagS = 0.02f;
// The actuator's frequency range, and therefore the DETECT band -- see the
// DETECT/FILLED band comment above. Changing either of these moves the detector
// floor/ceiling with it, which is the point: they are one number, not two.
static const float kMinCellFreqHz = 55.0f;
static const float kMaxCellFreqHz = 2000.0f;

// ------------------------------------------------ MASTER UPWARD UNIT
// Abel's request, 2026-09-14: one global gain, in dB, on the cell chain's
// output, ahead of the safety ceiling.
//
// The cells themselves only ever CUT -- gainDb = -clamp(partialDb - targetDb, 0,
// maxCut) -- so on their own they cannot start a partial that is not already
// sustaining. What they do is stop a winner running away. ground-rules 5's
// homogeneous saturation is why that matters: the fastest-growing mode reaches
// the loop's first nonlinearity and pulls the WHOLE loop's gain down with it,
// pushing everything else below unity. Regulating the winner to a target undoes
// exactly that, and hands every other mode back its own open-loop loop gain.
//
// This control supplies the other half. A mode sustains when its round-trip loop
// gain -- pickup -> DSP -> DTA120 -> exciter -> body -> string -> pickup --
// reaches unity, and this multiplies that gain for EVERY mode at once, so modes
// that sat below unity are lifted towards it and modes too quiet to be detected
// at all rise into candidate range. The cells then hold whichever ones take off
// at the target instead of letting one of them saturate the loop again. Turning
// this up is the direct lever on how many partials sustain.
//
// WHERE IT SITS MATTERS, and it goes AHEAD of the cells -- on the signal the
// detectors and the STFT both read, not on the cell chain's output. Put it after
// them and the cells regulate a partial to the target and the master then
// multiplies what they just regulated, which they cannot see or correct: the
// output leaves the target by exactly the master setting and walks into the
// ceiling. Ahead of them, a bound partial is still regulated to the target no
// matter how high this goes, and the lift survives only on partials no cell has
// taken -- which is precisely the population it is supposed to act on.
//
// Measured in host/harness/loop_sim.py over the peaked six-mode plant, sustained
// modes and the fraction of output samples hitting the ceiling:
//
//     master   after the cells      ahead of the cells
//      +3 dB    3/6    0.00 %         3/6    0.00 %
//      +6 dB    3/6    1.60 %         4/6    0.00 %
//      +9 dB    4/6   24.31 %         5/6    0.00 %
//     +12 dB    4/6   50.96 %         5/6    0.00 %
//     +18 dB    5/6   82.17 %         6/6    0.00 %
//
// Ahead of the cells is better on both counts at every setting, and stops
// clipping being the price of recruiting another mode.
//
// It is still blunt: being broadband it changes no partial's rank, so it lifts
// the noise floor along with everything else, and every dB of it is a dB the
// cells must spend from kMaxCutDb to hold the winners down. Expect the useful
// setting to be bounded by those two things rather than by the slider's range.
static const float kMasterBoostDb = 0.0f;

// Safety clamps applied in code to every live-tuning control, regardless of
// what the browser sends -- defense in depth, not just trusting the slider's
// own min/max. None of these touch kOutputCeiling (the actual rule-1 safety
// ceiling) or kWatchdogTimeoutS -- those stay fixed constants, full stop, not
// reachable from the GUI at all. (The watchdog is disabled in this project as of
// 2026-09-17, but by editing that constant and rebuilding, which is exactly the
// "explicit human commit" route -- never from a slider.)
static const float kTargetDbMin = -48.0f, kTargetDbMax = -6.0f;
static const float kMaxCutDbMin = 3.0f,   kMaxCutDbMax = 40.0f;
static const float kReleaseMsMin = 50.0f, kReleaseMsMax = 3000.0f;
static const float kCellQMin = 2.0f,      kCellQMax = 30.0f;
// Upper limit raised from +18 to +30 dB, 2026-09-17, Abel: "experimenting with
// pushing the upward compression more hardly". Two things to know before living
// up there. This sits AHEAD of the cells, so every dB of it is a dB a cell must
// spend out of kMaxCutDb to hold a bound winner at the target -- run out and the
// winner escapes the target upwards; raise max_cut_db alongside it. And a partial
// NO cell has bound gets the full lift with nothing regulating it, so the higher
// this goes the more the output leans on clampToCeiling(). That clamp is rule 1's
// backstop and is unchanged, but a backstop doing musical work means this is set
// too high.
static const float kMasterBoostDbMin = -12.0f, kMasterBoostDbMax = 30.0f;
static const float kAttackMsMin = 1.0f, kAttackMsMax = 500.0f;
static const float kProminenceDbMin = 3.0f, kProminenceDbMax = 30.0f;
// Loop gain: a software multiplier on the cells' output, standing in for
// walking back to the DTA120 knob for every experiment. Deliberately capped
// well below what could matter for loop stability on its own -- the output
// ceiling clamp still applies unconditionally after this, every sample,
// regardless of what this is set to.
static const float kLoopGainDefault = 1.0f;
static const float kLoopGainMin = 0.0f, kLoopGainMax = 1.5f;

// 2026-09-13, Abel's request: steal (and an ordinary release-then-rebind in the
// SAME hop frame, which the allocator allows -- a cell can free and be reassigned
// before its audio-rate cut has decayed) both retarget a cell's centre frequency
// while it may still carry a substantial cut from its OLD partial. The frequency
// slew alone (kFreqLagS) glides the centre smoothly, but that just means the
// deep part of an old, unrelated cut sweeps smoothly THROUGH every frequency
// between the old and new target instead of jumping there -- still audible as a
// notch swiping across the signal, not a click exactly, but exactly the "loud
// sudden change" this exists to rule out. Fix: duck the amount of cut actually
// applied to 0 the instant a cell is rebound (gRebindEpoch changes), then ease it
// back in over kRebindDuckS once the new binding has had a moment to settle --
// covers a fresh bind and a steal with the same mechanism, deliberately slower
// than kFreqLagS so the notch is essentially gone before the frequency is still
// moving, and slow enough itself not to be its own click.
static const float kRebindDuckS = 0.05f;
// ...and the RAMP OUT, added 2026-09-17. kRebindDuckS above is the ramp back IN.
// Until now the way out was `gCellDuck[c] = 0.0f` -- a single-sample jump. A cell
// that rebinds while still carrying a substantial cut therefore removed that whole
// cut instantaneously: a step in the actuator's gain, i.e. a click, whose size is
// exactly the cut the cell was holding. That is why it is only audible when the
// system is pushed hard (big cuts) and during heavy bind/release churn (many
// rebinds) -- the two conditions Abel reported it under, 2026-09-17.
//
// The mechanism was right and the asymmetry was simply an oversight: ducking
// exists so a retarget does not sweep a live notch across the spectrum, and it
// cannot do that job by starting with a discontinuity of its own.
//
// It is also newly more frequent because of the collision-resolution pass added
// earlier today: that frees a colliding cell, and a freed cell rebinds quickly,
// so there are more rebinds per second now than there were before it landed.
//
// 15 ms is a filter gain morphing smoothly, not a transient -- even 30 dB over
// 15 ms is a fade, not an edge -- while being short enough that the stale notch
// on the old frequency is gone long before the new binding needs to act.
static const float kRebindDuckOutS = 0.015f;


static const int kCpuMonitoringAcquisitionBlocks = 1000;

static const std::string kInputsFilename = "inputs.wav";
static const std::string kOutputsFilename = "outputs.wav";
static const size_t kAudioFileBufferSize = 16384;

// ------------------------------------------------------------------- state

static bool     gMuted = false;
static uint64_t gFrameCount = 0;

static AudioFileWriter gInputWriter;
static AudioFileWriter gOutputWriter;
static std::vector<float> gInputBuf;
static std::vector<float> gOutputBuf;

static std::vector<float> gRing(kRingSize, 0.0f);
static int gRingWritePos = 0;
static int gSamplesSinceHop = 0;

static Fft gFft;
static std::vector<float> gWindowCoeff(kFftWindow);
static std::vector<float> gAnalysisFrame(kFftWindow);
static std::vector<float> gPrevMagDb(kNumBins, -200.0f);
static std::vector<float> gSmoothedGrowth(kNumBins, 0.0f);   // telemetry only
static std::vector<int>   gPersistence(kNumBins, 0);

static float gSampleRate = 44100.0f;
static AuxiliaryTask gAnalysisTask;

// The two bands -- see the DETECT/FILLED band comment in the constants. Both set
// once in setup(). FILLED is what gets converted to dB (the optimisation that
// pays for the 8192 window); DETECT is what may hold a peak (the detector floor).
static int gAnalysisLoBin = 0;
static int gAnalysisHiBin = kNumBins - 1;
static int gDetectLoBin = 0;
static int gDetectHiBin = kNumBins - 1;

// Scratch for one analysis frame. Static rather than a local std::vector: at
// kNumBins = 4097 a per-hop local would mean a heap allocation every 5.8 ms.
// Only the auxiliary task ever touches it, and only one instance of that task
// runs at a time, so a single shared buffer is safe.
static std::vector<float> gMagDb(kNumBins, -200.0f);

// kPeakDecayDbPerSecond converted to dB per STFT hop; set in setup().
static float gPeakDecayDbPerFrame = 0.0f;

// ---- allocator state, one slot per cell. Written only by the auxiliary
// task (except gCellCutDbLast, written only by the audio thread) -- same
// single-writer convention bela/detector-passthrough and bela/gen1-cell rely
// on already.
enum CellState { kFree = 0, kBound = 1 };
static std::array<CellState, kNumCells> gCellState;
static std::array<int,   kNumCells> gBoundBin;
static std::array<int,   kNumCells> gBoundFrames;
static std::array<float, kNumCells> gBoundPeakMagDb;
static std::array<int,   kNumCells> gBelowReleaseFrames;
static std::array<float, kNumCells> gCandidateFreqHz;
static std::array<int,   kNumCells> gLockoutBin;
static std::array<int,   kNumCells> gLockoutFrames;
static unsigned int gBindEventsTotal = 0;
static unsigned int gReleaseEventsTotal = 0;
static unsigned int gStealEventsTotal = 0;

// Bumped every time a cell's bound_bin is assigned a NEW target -- a fresh
// bind from FREE, or a steal. NOT bumped by an ordinary glide (same
// partial, bin moves by at most the glide radius) -- glide is already
// continuous and needs no extra safety. The audio thread watches this to
// know when to duck a cell's cut down and back in (see kRebindDuckS and
// gCellDuck below) -- otherwise a steal or a same-frame release-then-rebind
// can retarget a cell's actuator to an unrelated frequency while it still
// carries its old partial's cut, sweeping an audible notch across whatever
// is between the two frequencies. Cross-thread like every other allocator
// array here: aux task writes, audio thread reads.
static std::array<unsigned int, kNumCells> gRebindEpoch{};

// ---- audio-rate cell DSP
static std::array<float, kNumCells> gFreqSlewed;
static float gFreqLagCoeff = 0.0f;
static std::array<Biquad, kNumCells> gDetectBpf;
static std::array<Biquad, kNumCells> gActuatorEq;
static std::array<EnvelopeDetector, kNumCells> gEnvelope;
static std::array<float, kNumCells> gCellCutDbLast{};   // audio thread writes, aux
                                                         // task reads (steal decision).

// Rebind duck (see kRebindDuckS): 0 right after a fresh bind/steal, ramps to 1
// over kRebindDuckS -- multiplies the cut actually handed to the actuator, not
// the computed cutDb itself (so telemetry/steal decisions still see the real
// value). Starts at 1 so a cell that has never been rebound applies no duck.
static std::array<float, kNumCells> gCellDuck{};
static std::array<unsigned int, kNumCells> gLastSeenRebindEpoch{};
static float gDuckIncrement = 0.0f;
// True from the moment a retarget is seen until the duck has reached 0. While it
// is set, the cell's centre frequency is HELD: the whole point of ducking out is
// that the cut leaves before the frequency moves, and letting the frequency start
// travelling immediately (as it did before 2026-09-17) is what the duck exists to
// prevent. Sequence per retarget: ramp cut out -> release the frequency -> slew to
// the new partial -> ramp cut back in.
static std::array<unsigned char, kNumCells> gCellRetargetPending{};
static float gDuckOutDecrement = 0.0f;


static BelaCpuData* gCpuData = nullptr;
static Scope gScope;


// ------------------------------------------------------------------ helpers

static inline float clampToCeiling(float x)
{
	if(x >  kOutputCeiling) return  kOutputCeiling;
	if(x < -kOutputCeiling) return -kOutputCeiling;
	return x;
}

static inline float clampf(float x, float lo, float hi)
{
	if(x < lo) return lo;
	if(x > hi) return hi;
	return x;
}

// How many bins away from bin `i` is `cents` cents, at this window size? Bin
// index is linear in frequency and cents are logarithmic, so this is a function
// of where in the spectrum you are -- which is exactly why a fixed bin radius
// could not express ground-rules 6.2's glide window. See kGlideCents.
static inline int centsToBins(int bin, float cents, int minBins)
{
	const float factor = powf(2.0f, cents / 1200.0f) - 1.0f;
	const int r = (int)lrintf((float)bin * factor);
	return (r < minBins) ? minBins : r;
}

// Same shape as bela/gen1-cell's isProminentPeak(), but the threshold is now a
// live parameter rather than a compile-time 25 dB -- see kProminenceDbStart for
// why that constant was tuned for the opposite problem to the one we have.
// Bounded to the DETECT band, not the filled one -- see the DETECT/FILLED band
// comment in the constants for the 86 Hz floor this fixes. setup() guarantees
// the shoulder taps below stay inside the filled band for every bin in it.
static bool isProminentPeak(const std::vector<float>& magDb, int i, float prominenceDb)
{
	if(i < gDetectLoBin || i > gDetectHiBin)
		return false;
	if(magDb[i] < magDb[i - 1] || magDb[i] < magDb[i + 1])
		return false;

	const float shoulderLeft  = 0.5f * (magDb[i - kNeighborOffsetBins] + magDb[i - kNeighborOffsetBins * 2]);
	const float shoulderRight = 0.5f * (magDb[i + kNeighborOffsetBins] + magDb[i + kNeighborOffsetBins * 2]);
	const float shoulder = std::max(shoulderLeft, shoulderRight);
	return (magDb[i] - shoulder) >= prominenceDb;
}

// Identical to bela/gen1-cell's interpolatedBinToHz().
static float interpolatedBinToHz(const std::vector<float>& magDb, int i)
{
	const float alpha = magDb[i - 1];
	const float beta  = magDb[i];
	const float gamma = magDb[i + 1];
	const float denom = (alpha - 2.0f * beta + gamma);
	float p = 0.0f;
	if(std::fabs(denom) > 1e-9f) {
		p = 0.5f * (alpha - gamma) / denom;
		p = clampf(p, -0.5f, 0.5f);
	}
	return (i + p) * gSampleRate / (float)kFftWindow;
}

// Is bin `i` within kExclusionCents of any currently-bound or
// lockout-held bin? Used to keep two cells from binding to the same (or an
// adjacent, effectively-the-same) partial.
static bool isBinOccupied(int i)
{
	const int radius = centsToBins(i, kExclusionCents, kExclusionBinRadiusMin);
	for(int c = 0; c < kNumCells; c++) {
		if(gCellState[c] == kBound && std::abs(i - gBoundBin[c]) <= radius)
			return true;
		if(gLockoutFrames[c] > 0 && std::abs(i - gLockoutBin[c]) <= radius)
			return true;
	}
	return false;
}

// The auxiliary task: STFT + the N-cell allocator (bind / glide / release /
// steal). Lower priority than the audio thread, scheduled once per hop --
// never on the audio thread itself.
void analyseFrame(void*)
{
	int readPos = (gRingWritePos - kFftWindow + kRingSize) % kRingSize;
	for(int n = 0; n < kFftWindow; n++) {
		gAnalysisFrame[n] = gRing[readPos] * gWindowCoeff[n];
		readPos++;
		if(readPos >= kRingSize) readPos = 0;
	}

	gFft.fft(gAnalysisFrame);

	// Only the FILLED band, not all kNumBins -- see the DETECT/FILLED band
	// comment in the constants. This is
	// what keeps the 8192-point window cheaper per hop than the 2048 one was:
	// ~430 log10f calls instead of 1025.
	std::vector<float>& magDb = gMagDb;
	const float prominenceDb = clampf(gWatchProminenceDb.get(), kProminenceDbMin, kProminenceDbMax);
	const float minHoldMs = clampf(gWatchMinHoldMs.get(), kMinHoldMsMin, kMinHoldMsMax);
	const float releaseMarginDb = clampf(gWatchReleaseMarginDb.get(),
	                                     kReleaseMarginDbMin, kReleaseMarginDbMax);
	const int minHoldFrames = (int)(0.001f * minHoldMs * gSampleRate / (float)kHop);
	for(int i = gAnalysisLoBin; i <= gAnalysisHiBin; i++) {
		const float mag = gFft.fda(i) / (kFftWindow / 2.0f);
		magDb[i] = 20.0f * log10f(std::max(mag, 1e-12f));
	}
	for(int i = gAnalysisLoBin; i <= gAnalysisHiBin; i++) {
		const float growth = magDb[i] - gPrevMagDb[i];   // telemetry only, see header
		gSmoothedGrowth[i] = kGrowthSmoothing * growth + (1.0f - kGrowthSmoothing) * gSmoothedGrowth[i];
		gPersistence[i] = isProminentPeak(magDb, i, prominenceDb) ? gPersistence[i] + 1 : 0;
		gPrevMagDb[i] = magDb[i];
	}

	for(int c = 0; c < kNumCells; c++) {
		if(gLockoutFrames[c] > 0) gLockoutFrames[c]--;
	}

	// ---- glide + release for every currently-bound cell ------------------
	for(int c = 0; c < kNumCells; c++) {
		if(gCellState[c] != kBound) continue;

		gBoundFrames[c]++;

		int newBin = gBoundBin[c];
		float newBinMagDb = magDb[gBoundBin[c]];
		// Cents-based glide window (ground-rules 6.2's "+-~50 cents"), not a fixed
		// bin count -- see kGlideCents.
		const int glideRadius = centsToBins(gBoundBin[c], kGlideCents, kGlideBinRadiusMin);
		// Clamped to the DETECT band for the same reason isProminentPeak() is: a cell
		// must never glide somewhere its actuator cannot follow it to.
		const int lo = std::max(gDetectLoBin, gBoundBin[c] - glideRadius);
		const int hi = std::min(gDetectHiBin, gBoundBin[c] + glideRadius);
		for(int i = lo; i <= hi; i++) {
			if(magDb[i] > newBinMagDb) {
				newBin = i;
				newBinMagDb = magDb[i];
			}
		}
		gBoundBin[c] = newBin;
		gCandidateFreqHz[c] = interpolatedBinToHz(magDb, newBin);
		// Decaying high-water mark, not a running max -- see kPeakDecayDbPerSecond
		// for why a latched one released every cell just after every pluck.
		gBoundPeakMagDb[c] -= gPeakDecayDbPerFrame;
		gBoundPeakMagDb[c] = std::max(gBoundPeakMagDb[c], newBinMagDb);

		if(gBoundFrames[c] > minHoldFrames) {
			if(newBinMagDb <= gBoundPeakMagDb[c] - releaseMarginDb
			   || newBinMagDb < kReleaseFloorDbfs) {
				gBelowReleaseFrames[c]++;
			} else {
				gBelowReleaseFrames[c] = 0;
			}
			if(gBelowReleaseFrames[c] >= kReleaseHoldFrames) {
				gCellState[c] = kFree;
				gLockoutBin[c] = gBoundBin[c];
				gLockoutFrames[c] = kLockoutFrames;
				gBoundBin[c] = -1;
				gReleaseEventsTotal++;
			}
		}
	}

	// ---- collision resolution: two bound cells on the same partial -------
	// 2026-09-17, observed live: cells 0 and 11 both sat on 80 Hz (cut 10.9 /
	// 10.8 dB) and cells 6 and 9 both on 734 Hz (cut 3.5 / 3.5, level identical
	// to the decimal). kExclusionCents was enforced in exactly ONE place --
	// isBinOccupied() at candidate-gather time -- which covers binding and
	// nothing else. The glide above has no occupancy check at all: it walks a
	// cell to whichever bin in +-kGlideCents is loudest, so once the low E
	// became the loudest thing in the spectrum (which is what the detector-floor
	// fix earlier today accomplished), any cell that drifted within glide range
	// of it climbed that slope and collided with the cell already there. A
	// sidelobe of a strong partial is enough to start the walk: it binds outside
	// the keep-out radius, then glides uphill into the peak over a few frames.
	//
	// This is not just a wasted cell. Every cell detects from `src` -- the raw,
	// pre-cell input -- so two cells on one partial each compute the FULL
	// correction and both apply it. The partial gets cut twice, the control
	// loop's gain is doubled, and the partial is driven well below the target.
	// On the flattening goal that is backwards: the doubled partial ends up too
	// quiet, and the cell that should have been regulating some other partial is
	// not regulating anything.
	//
	// The newcomer gives way, not the least-active one. The steal rule uses
	// least-active (ground-rules 6.2), but for a collision the two cuts are
	// near-identical by construction -- they are measuring the same partial --
	// so that tiebreak is arbitrary and would flip frame to frame. Bound-age is
	// decisive and stable: the incumbent has been regulating this partial, the
	// one that glided in is the intruder. Freeing it costs nothing audible --
	// a freed cell runs the ordinary envelope release, so its cut decays over
	// release_ms rather than jumping -- and the bind loop below can hand it a
	// real partial in this same frame.
	for(int a = 0; a < kNumCells; a++) {
		if(gCellState[a] != kBound) continue;
		for(int b = a + 1; b < kNumCells; b++) {
			if(gCellState[b] != kBound) continue;
			// Symmetric radius so the outcome cannot depend on loop order.
			const int radius = std::max(centsToBins(gBoundBin[a], kExclusionCents, kExclusionBinRadiusMin),
			                            centsToBins(gBoundBin[b], kExclusionCents, kExclusionBinRadiusMin));
			if(std::abs(gBoundBin[a] - gBoundBin[b]) > radius) continue;
			const int loser = (gBoundFrames[a] < gBoundFrames[b]) ? a : b;
			gCellState[loser] = kFree;
			gLockoutBin[loser] = gBoundBin[loser];
			gLockoutFrames[loser] = kLockoutFrames;
			gBoundBin[loser] = -1;
			gReleaseEventsTotal++;
			if(loser == a) break;   // `a` is free now; nothing left to compare it against
		}
	}

	// ---- gather candidates: stable, prominent, above the floor, not ------
	// ---- already occupied by a bound or lockout-held cell ----------------
	struct Candidate { int bin; float magDb; };
	static std::vector<Candidate> candidates;   // static: reused, never reallocates
	                                             // after the first few frames.
	candidates.clear();
	for(int i = gDetectLoBin; i <= gDetectHiBin; i++) {
		if(isProminentPeak(magDb, i, prominenceDb) && magDb[i] > kSilenceDbfs
		   && gPersistence[i] >= kStableHoldFrames && !isBinOccupied(i)) {
			candidates.push_back({i, magDb[i]});
		}
	}
	std::sort(candidates.begin(), candidates.end(),
	          [](const Candidate& a, const Candidate& b) { return a.magDb > b.magDb; });

	// ---- bind free cells to the loudest candidates ------------------------
	size_t nextCandidate = 0;
	for(int c = 0; c < kNumCells && nextCandidate < candidates.size(); c++) {
		if(gCellState[c] != kFree) continue;
		// The candidate list was filtered against occupancy ONCE, at gather time,
		// and its entries are not mutually exclusive with each other: two peaks
		// within kExclusionCents can both be in it, and before 2026-09-17 both
		// would bind, putting two cells on one partial in a single frame. Re-test
		// here -- isBinOccupied() reads gCellState/gBoundBin, which the iterations
		// above have already updated, so this sees cells bound earlier in this very
		// loop. Same guard, applied at the moment it actually decides something.
		while(nextCandidate < candidates.size() && isBinOccupied(candidates[nextCandidate].bin))
			nextCandidate++;
		if(nextCandidate >= candidates.size()) break;
		const Candidate& cand = candidates[nextCandidate++];
		gCellState[c] = kBound;
		gBoundBin[c] = cand.bin;
		gBoundFrames[c] = 0;
		gBoundPeakMagDb[c] = cand.magDb;
		gBelowReleaseFrames[c] = 0;
		gCandidateFreqHz[c] = interpolatedBinToHz(magDb, cand.bin);
		gRebindEpoch[c]++;
		gBindEventsTotal++;
	}

	// ---- steal: no cells free, but a candidate is still left over --------
	// Ground-rules 6.2: steal the cell whose partial is LEAST ACTIVE (lowest
	// current gain reduction), not the oldest. Only cells past their own
	// anti-chatter min-hold are eligible, same guard release uses.
	while(nextCandidate < candidates.size() && isBinOccupied(candidates[nextCandidate].bin))
		nextCandidate++;   // same re-test as the bind loop, for the steal challenger
	if(nextCandidate < candidates.size()) {
		// Two passes, because "least active" has to cope with ties -- see
		// kStealCutTieDb. Pass 1 finds the smallest current cut; pass 2 picks,
		// among every cell within kStealCutTieDb of it, the one whose partial is
		// quietest. With cuts that genuinely differ this is identical to the old
		// strict minimum; with several cells at 0.0 dB it replaces an index-order
		// accident with the actual least-valuable slot.
		int stealTarget = -1;
		float lowestCutDb = 1e9f;
		for(int c = 0; c < kNumCells; c++) {
			if(gCellState[c] != kBound || gBoundFrames[c] <= minHoldFrames) continue;
			if(gCellCutDbLast[c] < lowestCutDb) lowestCutDb = gCellCutDbLast[c];
		}
		float lowestMagDb = 1e9f;
		for(int c = 0; c < kNumCells; c++) {
			if(gCellState[c] != kBound || gBoundFrames[c] <= minHoldFrames) continue;
			if(gCellCutDbLast[c] > lowestCutDb + kStealCutTieDb) continue;
			if(magDb[gBoundBin[c]] < lowestMagDb) {
				lowestMagDb = magDb[gBoundBin[c]];
				stealTarget = c;
			}
		}
		// A steal must be WORTH it. Until 2026-09-14 this was unconditional: the
		// moment all 12 cells were bound and any candidate at all was left over,
		// some cell got ripped off its partial. Replayed against the recorded
		// takes that costs ~725 steals in 33 s on first-cell-take and ~3590 in
		// 123 s on feedback_ramp_signal_75pct -- a steal every 30-50 ms, i.e. the
		// pool thrashing rather than holding anything. Lowering the prominence bar
		// to 10 dB makes it worse, not better, because far more candidates now
		// qualify. host/harness/multicell_sandbox.py already proposed exactly this
		// guard (its STEAL_MARGIN_DB, defaulted to 0 there so it reproduced
		// render.cpp); this is that fix, landed, with a real margin.
		//
		// The rule: only steal if the challenger is beating the incumbent's
		// CURRENT level by kStealMarginDb. Ranking by "least active cell" alone
		// says which cell is cheapest to take, never whether taking it is an
		// improvement -- and with 12 cells bound there is always a cheapest one.
		if(stealTarget >= 0
		   && candidates[nextCandidate].magDb < magDb[gBoundBin[stealTarget]] + kStealMarginDb) {
			stealTarget = -1;
		}
		if(stealTarget >= 0) {
			const Candidate& cand = candidates[nextCandidate];
			gLockoutBin[stealTarget] = gBoundBin[stealTarget];
			gLockoutFrames[stealTarget] = kLockoutFrames;
			gBoundBin[stealTarget] = cand.bin;
			gBoundFrames[stealTarget] = 0;
			gBoundPeakMagDb[stealTarget] = cand.magDb;
			gBelowReleaseFrames[stealTarget] = 0;
			gCandidateFreqHz[stealTarget] = interpolatedBinToHz(magDb, cand.bin);
			gRebindEpoch[stealTarget]++;
			gStealEventsTotal++;
			// gCellState[stealTarget] is already kBound -- steal does not free/rebind,
			// it just redirects. The frequency slew glides it there smoothly, but that
			// alone isn't enough -- see kRebindDuckS's header comment for why the cut
			// also needs ducking through a retarget, not just the frequency.
		}
	}
}

// ------------------------------------------------------------------- setup

bool setup(BelaContext *context, void *userData)
{
	gMuted = false;
	gFrameCount = 0;
	gRingWritePos = 0;
	gSamplesSinceHop = 0;
	gSampleRate = context->audioSampleRate;

	gCellState.fill(kFree);
	gBoundBin.fill(-1);
	gBoundFrames.fill(0);
	gBoundPeakMagDb.fill(-200.0f);
	gBelowReleaseFrames.fill(0);
	gCandidateFreqHz.fill(220.0f);
	gLockoutBin.fill(-1);
	gLockoutFrames.fill(0);
	gBindEventsTotal = 0;
	gReleaseEventsTotal = 0;
	gStealEventsTotal = 0;

	gFreqSlewed.fill(220.0f);
	gFreqLagCoeff = 1.0f - expf(-1.0f / (kFreqLagS * gSampleRate));
	gCellCutDbLast.fill(0.0f);
	gCellDuck.fill(1.0f);
	gCellRetargetPending.fill(0);
	gLastSeenRebindEpoch.fill(0u);
	gRebindEpoch.fill(0u);
	gDuckIncrement = 1.0f / (kRebindDuckS * gSampleRate);
	gDuckOutDecrement = 1.0f / (kRebindDuckOutS * gSampleRate);

	gPeakDecayDbPerFrame = kPeakDecayDbPerSecond * (float)kHop / gSampleRate;

	// The DETECT band is the actuator's own range; the FILLED band is that plus a
	// shoulder span at each end. See the DETECT/FILLED band comment in the
	// constants. Order matters: derive both, then re-clamp DETECT to what FILLED
	// can support, so isProminentPeak()'s +-kNeighborOffsetBins*2 taps are always
	// on bins that were actually converted to dB -- at any window or sample rate.
	gDetectLoBin   = (int)floorf(kMinCellFreqHz * (float)kFftWindow / gSampleRate);
	gDetectHiBin   = (int)ceilf(kMaxCellFreqHz * (float)kFftWindow / gSampleRate);
	gAnalysisLoBin = std::max(1, gDetectLoBin - kNeighborOffsetBins * 2);
	gAnalysisHiBin = std::min(kNumBins - 2, gDetectHiBin + kNeighborOffsetBins * 2);
	gDetectLoBin   = std::max(gDetectLoBin, gAnalysisLoBin + kNeighborOffsetBins * 2);
	gDetectHiBin   = std::min(gDetectHiBin, gAnalysisHiBin - kNeighborOffsetBins * 2);

	if(gFft.setup(kFftWindow)) {
		rt_printf("error setting up Fft\n");
		return false;
	}
	for(int n = 0; n < kFftWindow; n++) {
		gWindowCoeff[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (kFftWindow - 1)));
	}

	gAnalysisTask = Bela_createAuxiliaryTask(&analyseFrame, 85, "gen1-multicell-live-analysis");
	if(!gAnalysisTask) {
		rt_printf("error creating the analysis auxiliary task\n");
		return false;
	}

	for(int c = 0; c < kNumCells; c++) {
		BiquadCoeff::Settings bpfSettings {
			.fs = (double)gSampleRate, .type = BiquadCoeff::bandpass,
			.cutoff = gFreqSlewed[c], .q = kCellQ, .peakGainDb = 0.0
		};
		gDetectBpf[c].setup(bpfSettings);

		BiquadCoeff::Settings eqSettings {
			.fs = (double)gSampleRate, .type = BiquadCoeff::peak,
			.cutoff = gFreqSlewed[c], .q = kCellQ, .peakGainDb = 0.0
		};
		gActuatorEq[c].setup(eqSettings);

		gEnvelope[c].setup(kAttackMs, kReleaseMs, gSampleRate,
		                    EnvelopeDetector::ANALOG, EnvelopeDetector::BRANCHING,
		                    EnvelopeDetector::PEAK, true);
	}

	gWatchCellBound.reserve(kNumCells);
	gWatchCellFreqHz.reserve(kNumCells);
	gWatchCellPartialDb.reserve(kNumCells);
	gWatchCellCutDb.reserve(kNumCells);
	for(int c = 0; c < kNumCells; c++) {
		const std::string idx = std::to_string(c);
		gWatchCellBound.emplace_back("cell" + idx + "_bound");
		gWatchCellFreqHz.emplace_back("cell" + idx + "_freq_hz");
		gWatchCellPartialDb.emplace_back("cell" + idx + "_partial_db");
		gWatchCellCutDb.emplace_back("cell" + idx + "_cut_db");
	}

	Bela_cpuMonitoringInit(kCpuMonitoringAcquisitionBlocks);
	gCpuData = Bela_cpuMonitoringGet();

	if(gScope.setup(kScopeNumChannels, context->audioSampleRate)) {
		rt_printf("error setting up Scope\n");
		return false;
	}

	Bela_getDefaultWatcherManager()->getGui().setup(context->projectName);
	Bela_getDefaultWatcherManager()->setup(context->audioSampleRate);

	// localControl(false) is what actually makes a Watcher variable settable from
	// the browser: without it, .get() always returns whatever the C++ side last
	// assigned (`v`), never the remotely-set value (`vr`) a browser "set" command
	// writes -- see Watcher.h's own get()/wmSet(). Assign the default BEFORE
	// disabling local control (localControlChanged() copies v into vr at that
	// point), so the GUI's slider starts showing the real starting value, not 0.
	gWatchBypass = 0u;   // default: engaged. Host sets this to 1 for a quick live A/B.
	gWatchBypass.localControl(false);
	gWatchTargetDb = kTargetDb;
	gWatchTargetDb.localControl(false);
	gWatchMaxCutDb = kMaxCutDb;
	gWatchMaxCutDb.localControl(false);
	gWatchReleaseMs = kReleaseMs;
	gWatchReleaseMs.localControl(false);
	gWatchCellQ = kCellQ;
	gWatchCellQ.localControl(false);
	gWatchLoopGain = kLoopGainDefault;
	gWatchLoopGain.localControl(false);
	gWatchMasterBoostDb = kMasterBoostDb;
	gWatchMasterBoostDb.localControl(false);
	gWatchAttackMs = kAttackMs;
	gWatchAttackMs.localControl(false);
	gWatchSlope = kSlope;
	gWatchSlope.localControl(false);
	gWatchKneeDb = kKneeDb;
	gWatchKneeDb.localControl(false);
	gWatchProminenceDb = kProminenceDbStart;
	gWatchProminenceDb.localControl(false);
	gWatchMinHoldMs = 1000.0f * kMinBoundHoldFrames * (float)kHop / gSampleRate;
	gWatchMinHoldMs.localControl(false);
	gWatchReleaseMarginDb = kReleaseMarginDb;
	gWatchReleaseMarginDb.localControl(false);

	int ret = 0;
	ret |= gInputWriter.setup(kInputsFilename, kAudioFileBufferSize,
	                          context->audioInChannels, context->audioSampleRate);
	ret |= gOutputWriter.setup(kOutputsFilename, kAudioFileBufferSize,
	                           context->audioOutChannels, context->audioSampleRate);
	if(ret) {
		rt_printf("error opening %s / %s for recording\n",
		          kInputsFilename.c_str(), kOutputsFilename.c_str());
		return false;
	}
	gInputBuf.resize((size_t)context->audioInChannels * context->audioFrames);
	gOutputBuf.resize((size_t)context->audioOutChannels * context->audioFrames);

	rt_printf("gen1-multicell-live (N=%d)\n", kNumCells);
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	if(kWatchdogTimeoutS > 0.0f) {
		rt_printf("  ceiling: %.3f linear   watchdog: %.0f s   fade-in: %.2f s\n",
		          kOutputCeiling, kWatchdogTimeoutS, kFadeInS);
	} else {
		rt_printf("  ceiling: %.3f linear   watchdog: DISABLED   fade-in: %.2f s\n",
		          kOutputCeiling, kFadeInS);
		rt_printf("  *** no time box: this run does not stop on its own. Abel present "
		          "(rule 2) and the DTA120 power switch are the backstop. ***\n");
	}
	rt_printf("  analysis: window=%d hop=%d (%.1f ms, %.2f Hz/bin), "
	          "bind-by-magnitude, steal-least-active when full\n",
	          kFftWindow, kHop, 1000.0f * kHop / context->audioSampleRate,
	          context->audioSampleRate / (float)kFftWindow);
	// The DETECT floor/ceiling in Hz, computed, not asserted in a comment -- a
	// comment claiming 43 Hz over code that did 86 Hz is what hid the low E from
	// this detector for four days. Low E is 82.41 Hz: if the floor printed here is
	// above that, no cell can ever bind the lowest string.
	rt_printf("  detect band: %.1f-%.1f Hz (bins %d-%d)   filled band: %.1f-%.1f Hz "
	          "(bins %d-%d)   low E = 82.41 Hz\n",
	          gDetectLoBin * context->audioSampleRate / (float)kFftWindow,
	          gDetectHiBin * context->audioSampleRate / (float)kFftWindow,
	          gDetectLoBin, gDetectHiBin,
	          gAnalysisLoBin * context->audioSampleRate / (float)kFftWindow,
	          gAnalysisHiBin * context->audioSampleRate / (float)kFftWindow,
	          gAnalysisLoBin, gAnalysisHiBin);
	// Not a hard failure -- a higher floor could one day be deliberate -- but it is
	// never allowed to be silent again. The shoulder margin costs kNeighborOffsetBins*2
	// BINS, so its cost in Hz scales with the sample rate: at 96 kHz and this window
	// it is 94 Hz and the floor lands at 105 Hz, back above the low E. rig-profile.json
	// still lists 44.1/48/96 as an open Phase 0/1 choice, so this will be reached by
	// changing the rate alone, with no edit to this file. The fix if it fires is to
	// scale kFftWindow with the rate (same resolution in Hz), not to shave the margin.
	const float detectLoHz = gDetectLoBin * context->audioSampleRate / (float)kFftWindow;
	if(detectLoHz > 82.41f) {
		rt_printf("  *** WARNING: detect floor %.1f Hz is ABOVE the low E (82.41 Hz). "
		          "That string cannot bind a cell and will not be regulated. ***\n",
		          detectLoHz);
	}
	rt_printf("  cell: target=%.1f dB  Q=%.1f  attack=%.1f ms  release=%.0f ms  "
	          "maxCut=%.1f dB  freqLag=%.0f ms\n",
	          kTargetDb, kCellQ, kAttackMs, kReleaseMs, kMaxCutDb,
	          1000.0f * kFreqLagS);
	rt_printf("  master boost: %+.1f dB (ahead of the cells)   prominence: %.1f dB\n",
	          kMasterBoostDb, kProminenceDbStart);
	rt_printf("  reduction profile: slope=%+.2f dB/dB knee=%.1f dB "
	          "(slope 0 = brick wall at the target, <0 = over-compression)\n",
	          kSlope, kKneeDb);
	rt_printf("  recording: %s (in), %s (out)\n",
	          kInputsFilename.c_str(), kOutputsFilename.c_str());
	rt_printf("  bypass: %u (Watcher-settable; 1 = cells computed but not applied to "
	          "the audio path -- for an engaged/disengaged A/B)\n", gWatchBypass.get());

	return true;
}

// ------------------------------------------------------------------ render

void render(BelaContext *context, void *userData)
{
	const float elapsedS = (float)gFrameCount / context->audioSampleRate;

	// kWatchdogTimeoutS = 0 disables the time box entirely -- see that constant.
	// The mechanism stays exactly as it was so any positive value re-arms it.
	if(kWatchdogTimeoutS > 0.0f && !gMuted && elapsedS >= kWatchdogTimeoutS) {
		gMuted = true;
		rt_printf("watchdog timeout at %.1f s — output muted\n", elapsedS);
	}

	float fadeGain = 1.0f;
	if(elapsedS < kFadeInS) {
		fadeGain = elapsedS / kFadeInS;
		if(fadeGain < 0.0f) fadeGain = 0.0f;
	}

	float inPeak = 0.0f, outPeak = 0.0f;
	bool nonFiniteThisBlock = false;
	const bool bypass = (gWatchBypass.get() != 0u);

	// Live tuning controls -- read once per block (not per sample; a human
	// turning a slider doesn't need sample-accurate response, and this keeps
	// setQ()/setReleaseTime() calls, which aren't free, off the audio-rate
	// hot path). Clamped in code regardless of what the browser sends -- see
	// the constants' own header comment.
	const float targetDb = clampf(gWatchTargetDb.get(), kTargetDbMin, kTargetDbMax);
	const float maxCutDb = clampf(gWatchMaxCutDb.get(), kMaxCutDbMin, kMaxCutDbMax);
	const float releaseMs = clampf(gWatchReleaseMs.get(), kReleaseMsMin, kReleaseMsMax);
	const float attackMs = clampf(gWatchAttackMs.get(), kAttackMsMin, kAttackMsMax);
	const float cellQ = clampf(gWatchCellQ.get(), kCellQMin, kCellQMax);
	const float loopGain = clampf(gWatchLoopGain.get(), kLoopGainMin, kLoopGainMax);
	const float masterBoostDb = clampf(gWatchMasterBoostDb.get(), kMasterBoostDbMin, kMasterBoostDbMax);
	const float slope = clampf(gWatchSlope.get(), kSlopeMin, kSlopeMax);
	const float kneeDb = clampf(gWatchKneeDb.get(), kKneeDbMin, kKneeDbMax);
	const float slopeK = 1.0f - slope;   // cut = slopeK * excess, see REDUCTION PROFILE
	// The master upward unit -- applied to the input, AHEAD of the cells and of
	// every detector, for the reason set out at kMasterBoostDb. Not folded into
	// loopGain: that one is an output trim and stays where it is, after the cells.
	const float masterGain = powf(10.0f, masterBoostDb / 20.0f);

	static float sLastReleaseMs = kReleaseMs;
	if(releaseMs != sLastReleaseMs) {
		for(int c = 0; c < kNumCells; c++) gEnvelope[c].setReleaseTime(releaseMs);
		sLastReleaseMs = releaseMs;
	}
	static float sLastAttackMs = kAttackMs;
	if(attackMs != sLastAttackMs) {
		for(int c = 0; c < kNumCells; c++) gEnvelope[c].setAttackTime(attackMs);
		sLastAttackMs = attackMs;
	}
	static float sLastCellQ = kCellQ;
	if(cellQ != sLastCellQ) {
		for(int c = 0; c < kNumCells; c++) {
			gDetectBpf[c].setQ(cellQ);
			gActuatorEq[c].setQ(cellQ);
		}
		sLastCellQ = cellQ;
	}

	// gCellState doesn't change within one render() call (only the aux task
	// writes it, between calls) -- safe to compute once and republish every
	// sample below.
	unsigned int boundCount = 0;
	for(int c = 0; c < kNumCells; c++) {
		if(gCellState[c] == kBound) boundCount++;
	}

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		const uint64_t frames = context->audioFramesElapsed + n;
		Bela_getDefaultWatcherManager()->tick(frames);

		for(unsigned int ch = 0; ch < context->audioInChannels; ch++) {
			gInputBuf[n * context->audioInChannels + ch] = audioRead(context, n, ch);
		}
		const float in = audioRead(context, n, kGuitarInputChannel);
		const float a = std::fabs(in);
		if(a > inPeak) inPeak = a;   // the TRUE input peak, pre-master

		// Everything downstream of here -- the STFT that finds candidates, the
		// per-cell detectors, and the cell chain itself -- sees the post-master
		// signal, so that a cell's target still means the same thing at any master
		// setting. See kMasterBoostDb.
		const float src = in * masterGain;

		gRing[gRingWritePos] = src;
		gRingWritePos++;
		if(gRingWritePos >= kRingSize) gRingWritePos = 0;
		gSamplesSinceHop++;
		if(gSamplesSinceHop >= kHop) {
			gSamplesSinceHop = 0;
			Bela_scheduleAuxiliaryTask(gAnalysisTask);
		}

		float sig = src;
		for(int c = 0; c < kNumCells; c++) {
			// Rebind duck -- see kRebindDuckS / kRebindDuckOutS. Runs BEFORE the
			// frequency slew below, because it is what decides whether the frequency
			// is allowed to move this sample at all.
			//
			// A new epoch (fresh bind or steal) starts a duck-OUT; it does not zero
			// the duck outright, which was a single-sample gain step and the click
			// Abel reported. The frequency is held until the cut is fully out, then
			// released, then the cut ramps back in over kRebindDuckS.
			if(gLastSeenRebindEpoch[c] != gRebindEpoch[c]) {
				gLastSeenRebindEpoch[c] = gRebindEpoch[c];
				gCellRetargetPending[c] = 1;
			}
			if(gCellRetargetPending[c]) {
				gCellDuck[c] -= gDuckOutDecrement;
				if(gCellDuck[c] <= 0.0f) {
					gCellDuck[c] = 0.0f;
					gCellRetargetPending[c] = 0;   // the frequency may move from here
				}
			} else if(gCellDuck[c] < 1.0f) {
				gCellDuck[c] = std::min(1.0f, gCellDuck[c] + gDuckIncrement);
			}

			if(gCellState[c] == kBound && !gCellRetargetPending[c]) {
				gFreqSlewed[c] += (gCandidateFreqHz[c] - gFreqSlewed[c]) * gFreqLagCoeff;
			}
			const float freqNow = clampf(gFreqSlewed[c], kMinCellFreqHz, kMaxCellFreqHz);

			float envLin;
			if(gCellState[c] == kBound) {
				gDetectBpf[c].setFc(freqNow);
				const float partial = (float)gDetectBpf[c].process(src);
				envLin = gEnvelope[c].process(partial);
			} else {
				envLin = gEnvelope[c].process(0.0f);
			}
			const float partialDb = 20.0f * log10f(std::max(envLin, 1e-9f));

			// Reduction profile -- see REDUCTION PROFILE in the constants. With
			// slope 0 and knee 0 this is exactly clampf(partialDb - targetDb, 0,
			// maxCutDb), the law this file carried before.
			const float excessDb = partialDb - targetDb;
			float rawCutDb;
			if(kneeDb > 0.0f && excessDb > -0.5f * kneeDb && excessDb < 0.5f * kneeDb) {
				const float t = excessDb + 0.5f * kneeDb;
				rawCutDb = slopeK * t * t / (2.0f * kneeDb);
			} else {
				rawCutDb = slopeK * excessDb;
			}
			const float cutDb = clampf(rawCutDb, 0.0f, maxCutDb);
			const float appliedCutDb = cutDb * gCellDuck[c];

			gActuatorEq[c].setFc(freqNow);
			gActuatorEq[c].setPeakGain(-appliedCutDb);
			sig = (float)gActuatorEq[c].process(sig);

			if(!std::isfinite(freqNow) || !std::isfinite(cutDb) || !std::isfinite(sig)) {
				nonFiniteThisBlock = true;
			}

			gCellCutDbLast[c] = cutDb;

			// Published every sample, not once per block -- see the render()-top
			// comment (and the commit message) on why: Watcher/pybela's streaming
			// protocol turns out to expect audio-rate writes, not block-rate ones.
			gWatchCellBound[c] = (gCellState[c] == kBound) ? 1u : 0u;
			gWatchCellFreqHz[c] = freqNow;
			gWatchCellPartialDb[c] = partialDb;
			gWatchCellCutDb[c] = cutDb;
		}

		// bypass (Watcher-settable, see header note): cells above still ran in
		// full -- this only decides which signal reaches the exciter, exactly
		// mirroring sc/gen1_cell.scd's Select.ar(bypass, [wet, in]). Bypass takes
		// the RAW input, not the post-master one: the point of it is a dry
		// reference, and a master-boosted dry path would not be one.
		const float selected = (bypass ? in : sig) * loopGain;

		float out = 0.0f;
		if(!gMuted) {
			if(!std::isfinite(in) || !std::isfinite(selected) || nonFiniteThisBlock) {
				gMuted = true;
				rt_printf("non-finite sample — output muted\n");
			} else {
				// clampToCeiling is the actual safety backstop (rule 1) -- unconditional,
				// regardless of loopGain, bypass, or anything above.
				out = clampToCeiling(selected) * fadeGain;
			}
		}

		for(unsigned int ch = 0; ch < context->audioOutChannels; ch++) {
			audioWrite(context, n, ch, out);
			gOutputBuf[n * context->audioOutChannels + ch] = out;
		}
		const float ao = std::fabs(out);
		if(ao > outPeak) outPeak = ao;

		// Published every sample -- see the per-cell comment above for why.
		gWatchInPeak = inPeak;
		gWatchOutPeak = outPeak;
		gWatchMuted = gMuted ? 1u : 0u;
		gWatchBindEvents = gBindEventsTotal;
		gWatchReleaseEvents = gReleaseEventsTotal;
		gWatchStealEvents = gStealEventsTotal;
		gWatchCpuPercent = gCpuData ? gCpuData->percentage : 0.0f;   // already 0-100, see
		                                                              // RTAudio.cpp's Bela_cpuTic
		gWatchCellsBoundCount = boundCount;

		const float scopeVals[kScopeNumChannels] = {
			inPeak, outPeak, (float)boundCount, gCpuData ? gCpuData->percentage : 0.0f
		};
		gScope.log(scopeVals);
	}

	gInputWriter.setSamples(gInputBuf);
	gOutputWriter.setSamples(gOutputBuf);

	// Periodic CPU print -- phase-plan.md Phase 5: "watch CPU load as N grows".
	// Watcher exposes it for pybela too, but a plain rt_printf means it shows up
	// in a foreground run's own console without any streaming setup at all,
	// which is what a quick "is N=12 safe" check actually wants.
	static float sLastCpuPrintS = -1000.0f;
	if(gCpuData && elapsedS - sLastCpuPrintS >= 2.0f) {
		sLastCpuPrintS = elapsedS;
		rt_printf("[%.1fs] audio thread CPU: %.1f%%\n", elapsedS, gCpuData->percentage);
	}

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
