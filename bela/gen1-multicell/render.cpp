/*
 * gen1-multicell — Phase 5 of docs/phase-plan.md: the N-cell allocator
 * (ground-rules-and-facts.md 6.2), generalising bela/gen1-cell/'s single cell
 * (Phase 4, PASSED 2026-09-13 -- see that project's header for the actuator
 * shape and the safety net, both unchanged here) into a pool.
 *
 *     guitar -> Bela ch0 -> [ cell 0 ] -> [ cell 1 ] -> ... -> [ cell N-1 ] -> Bela ch1 -> DTA120 -> exciter
 *                              ^              ^                    ^
 *                  in -> [ STFT, aux task ] -> [ allocator: bind / glide / release / steal ]
 *
 * Each cell is its own peaking-EQ biquad + bandpass detector + envelope
 * follower, cascaded in series on the audio path -- a small multi-band
 * parametric EQ where each band's centre frequency and depth are driven
 * independently. Detection for every cell reads the RAW input, not the
 * output of earlier cells in the chain: what a cell regulates is decided
 * from what is actually coming in, not from what the cells ahead of it did
 * to the signal, matching sc/gen1_cell.scd's and gen1-cell's Pitch.kr(in) /
 * STFT(in) philosophy directly.
 *
 * ALLOCATOR (ground-rules 6.2), all per-frame in the STFT auxiliary task:
 *   - Each BOUND cell glides with its partial (+-kGlideBinRadius bins,
 *     quadratic-interpolated -- same as gen1-cell) and releases with a
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
 * SAFETY: kOutputCeiling and kWatchdogTimeoutS are safety constants in the
 * CLAUDE.md sense -- unchanged in meaning and value from gen1-cell /
 * gen1-passthrough / detector-passthrough. No automatic tuning process may
 * raise or extend them (ground rules 1, 3). Everything under "cell tuning
 * constants" and "allocator constants" below is a musical/DSP parameter, not
 * a safety constant.
 *
 * RUN HISTORY: two real-loop takes 2026-09-13 (DTA120=100%, Abel present, both
 * before the sweep-kick/bypass additions below existed) read much closer to
 * single-partial than gen1-cell's pass -- see
 * logs/2026-09-13/{192844,193307}_*multicell-take*.json and phase-plan.md's
 * Status block. `host/rig/snapshot_partials.py`'s time-windowed view of the
 * second (40s) take shows it WAS trending toward multiple partials by the
 * end (dominance dropping from 120 dB to 1.1 dB over the last ~15s) -- not
 * yet distinguished from a slower-than-gen1-cell but otherwise normal build-
 * up, versus a real difference from the exciter mount being secured since
 * (rig-profile v4) or from gen1-multicell's own behaviour.
 *
 * SWEEP-KICK AND BYPASS (added same day, for exactly that investigation
 * without needing a human to pluck a string at the right moment -- see the
 * constants' own header comment above "bypass and sweep-kick"): a short,
 * quiet swept tone seeds the loop automatically near the start of every run,
 * and a Watcher-settable `bypass` flag lets the same code run with the cells
 * computed-but-not-applied, for a same-code, same-seed engaged/disengaged
 * comparison.
 */

#include <algorithm>
#include <array>
#include <Bela.h>
#include <Watcher.h>
#include <libraries/Fft/Fft.h>
#include <libraries/AudioFile/AudioFile.h>
#include <libraries/Biquad/Biquad.h>
#include <libraries/EnvelopeDetector/EnvelopeDetector.h>
#include <cmath>
#include <string>
#include <vector>

// Phase 5 (phase-plan.md: "Cell pool N = 4, then 6-8") -- bumped straight to 12,
// 2026-09-13, Abel's request, past the plan's own 6-8 ceiling. CPU load checked
// live at this N before trusting it (see setup()'s periodic rt_printf and the
// commit message) rather than assumed safe just because it compiles.
static const int kNumCells = 12;

Watcher<float>        gWatchInPeak("in_peak");
Watcher<float>        gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");
Watcher<unsigned int> gWatchCellsBoundCount("cells_bound_count");
Watcher<unsigned int> gWatchBindEvents("bind_events_total");
Watcher<unsigned int> gWatchReleaseEvents("release_events_total");
Watcher<unsigned int> gWatchStealEvents("steal_events_total");
Watcher<float>        gWatchCpuPercent("audio_thread_cpu_percent");
Watcher<unsigned int> gWatchBypass("bypass");        // host-settable, see header note
Watcher<unsigned int> gWatchSweepActive("sweep_active");

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

// Safety constants -- see header comment. Identical meaning and value to
// gen1-cell / gen1-passthrough / detector-passthrough.
static const float kOutputCeiling = 0.5f;
static const float kWatchdogTimeoutS = 120.0f;
static const float kFadeInS = 0.2f;

static const unsigned int kGuitarInputChannel = 0;

// ---- bypass and sweep-kick (2026-09-13, Abel's suggestions for autonomous
// A/B testing without needing a human to pluck a string at the right
// moment). Neither is a safety constant.
//
// `bypass` (host-settable via Watcher, default 0/engaged): when 1, the N
// cells still run in full -- STFT, allocator, actuators, all of it -- but
// their output is not what reaches the exciter; the excitation signal goes
// straight through instead, exactly mirroring sc/gen1_cell.scd's own
// Select.ar(bypass, [wet, in]). This is what makes an engaged/disengaged
// comparison meaningful: same code, same seed signal, only this one
// selection point differs (CLAUDE.md rule 10 -- one variable at a time).
//
// The sweep: a short, quiet, frequency-swept tone added to the excitation
// signal at the start of a run, so the physical loop gets a "kick" to start
// feeding back from instead of waiting on a human's timing. Conservative on
// purpose -- short (2s), quiet (0.05 linear, an order of magnitude under the
// output ceiling), and swept only across a modest range of common guitar
// partial frequencies, not the cell's full 55-2000 Hz range. It is mixed
// into the signal fed to the cells/analysis/output, NOT into the raw
// inputs.wav recording -- that file keeps meaning "what the pickup actually
// sent" across every project in this repo (see rig/analyse_take.py's
// docstring).
static const float kSweepDurationS = 2.0f;
static const float kSweepStartHz = 150.0f;
static const float kSweepEndHz = 500.0f;
static const float kSweepAmpLinear = 0.05f;
static const float kSweepRampS = 0.05f;   // raised-cosine fade at each end of the
                                           // sweep itself, so it doesn't click on
                                           // its own way in or out.

// ---- analysis (STFT, reused from bela/gen1-cell -- see that project's
// header for the fuller rationale, unchanged here).
static const int   kFftWindow = 2048;
static const int   kHop = 256;
static const int   kNumBins = kFftWindow / 2 + 1;
static const int   kRingSize = kFftWindow * 4;
static const float kGrowthSmoothing = 0.6f;      // telemetry only, see header note
static const int   kStableHoldFrames = 2;
static const float kReleaseMarginDb = 10.0f;
static const int   kReleaseHoldFrames = 8;
static const float kSilenceDbfs = -80.0f;
static const float kProminenceDb = 25.0f;        // see detector-passthrough's header --
                                                  // 25 dB reproduced zero false arms
                                                  // against real recorded room noise.
static const int   kNeighborOffsetBins = 4;
static const int   kGlideBinRadius = 2;          // ground-rules 6.2's "~50 cents" glide
                                                  // window, in bins -- see gen1-cell's
                                                  // header on why bins rather than cents.
static const int   kExclusionBinRadius = 6;      // how close a new candidate may be to
                                                  // an already-occupied (bound or
                                                  // lockout) bin before it is treated as
                                                  // the same partial rather than a
                                                  // distinct one worth a cell of its own.
static const int   kMinBoundHoldFrames = 40;     // anti-chatter, ~232 ms at hop 256 --
                                                  // guards both release and being stolen.
static const int   kLockoutFrames = 20;          // ground-rules 6.2's "short lockout
                                                  // before the same frequency region can
                                                  // be re-allocated", ~116 ms.

// ---- cell / actuator tuning (ground-rules 6.3; same defaults as gen1-cell,
// carried over untouched -- untuned, see that project's header).
static const float kTargetDb = -24.0f;
static const float kCellQ = 10.0f;
static const float kAttackMs = 3.0f;
static const float kReleaseMs = 400.0f;
static const float kMaxCutDb = 30.0f;
static const float kFreqLagS = 0.02f;
static const float kMinCellFreqHz = 55.0f;
static const float kMaxCellFreqHz = 2000.0f;

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
// partial, bin moves by at most kGlideBinRadius) -- glide is already
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

static BelaCpuData* gCpuData = nullptr;

static double gSweepPhase = 0.0;   // continuous phase accumulator, double for the
                                    // same reason oscillator phase accumulators
                                    // usually are: avoids audible drift over
                                    // kSweepDurationS at audio-rate increments.

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

// Identical to bela/gen1-cell's isProminentPeak() -- see that project's
// header comment for why 25 dB, not the Python reference's 6 dB.
static bool isProminentPeak(const std::vector<float>& magDb, int i)
{
	if(i < kNeighborOffsetBins * 2 || i >= kNumBins - kNeighborOffsetBins * 2)
		return false;
	if(magDb[i] < magDb[i - 1] || magDb[i] < magDb[i + 1])
		return false;

	const float shoulderLeft  = 0.5f * (magDb[i - kNeighborOffsetBins] + magDb[i - kNeighborOffsetBins * 2]);
	const float shoulderRight = 0.5f * (magDb[i + kNeighborOffsetBins] + magDb[i + kNeighborOffsetBins * 2]);
	const float shoulder = std::max(shoulderLeft, shoulderRight);
	return (magDb[i] - shoulder) >= kProminenceDb;
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

// Is bin `i` within kExclusionBinRadius of any currently-bound or
// lockout-held bin? Used to keep two cells from binding to the same (or an
// adjacent, effectively-the-same) partial.
static bool isBinOccupied(int i)
{
	for(int c = 0; c < kNumCells; c++) {
		if(gCellState[c] == kBound && std::abs(i - gBoundBin[c]) <= kExclusionBinRadius)
			return true;
		if(gLockoutFrames[c] > 0 && std::abs(i - gLockoutBin[c]) <= kExclusionBinRadius)
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

	std::vector<float> magDb(kNumBins);
	for(int i = 0; i < kNumBins; i++) {
		const float mag = gFft.fda(i) / (kFftWindow / 2.0f);
		magDb[i] = 20.0f * log10f(std::max(mag, 1e-12f));
	}
	for(int i = 0; i < kNumBins; i++) {
		const float growth = magDb[i] - gPrevMagDb[i];   // telemetry only, see header
		gSmoothedGrowth[i] = kGrowthSmoothing * growth + (1.0f - kGrowthSmoothing) * gSmoothedGrowth[i];
		gPersistence[i] = isProminentPeak(magDb, i) ? gPersistence[i] + 1 : 0;
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
		const int lo = std::max(kNeighborOffsetBins * 2, gBoundBin[c] - kGlideBinRadius);
		const int hi = std::min(kNumBins - kNeighborOffsetBins * 2 - 1, gBoundBin[c] + kGlideBinRadius);
		for(int i = lo; i <= hi; i++) {
			if(magDb[i] > newBinMagDb) {
				newBin = i;
				newBinMagDb = magDb[i];
			}
		}
		gBoundBin[c] = newBin;
		gCandidateFreqHz[c] = interpolatedBinToHz(magDb, newBin);
		gBoundPeakMagDb[c] = std::max(gBoundPeakMagDb[c], newBinMagDb);

		if(gBoundFrames[c] > kMinBoundHoldFrames) {
			if(newBinMagDb <= gBoundPeakMagDb[c] - kReleaseMarginDb) {
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

	// ---- gather candidates: stable, prominent, above the floor, not ------
	// ---- already occupied by a bound or lockout-held cell ----------------
	struct Candidate { int bin; float magDb; };
	std::vector<Candidate> candidates;
	for(int i = 0; i < kNumBins; i++) {
		if(isProminentPeak(magDb, i) && magDb[i] > kSilenceDbfs
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
	if(nextCandidate < candidates.size()) {
		int stealTarget = -1;
		float lowestCutDb = 1e9f;
		for(int c = 0; c < kNumCells; c++) {
			if(gCellState[c] != kBound || gBoundFrames[c] <= kMinBoundHoldFrames) continue;
			if(gCellCutDbLast[c] < lowestCutDb) {
				lowestCutDb = gCellCutDbLast[c];
				stealTarget = c;
			}
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
	gLastSeenRebindEpoch.fill(0u);
	gRebindEpoch.fill(0u);
	gDuckIncrement = 1.0f / (kRebindDuckS * gSampleRate);
	gSweepPhase = 0.0;

	if(gFft.setup(kFftWindow)) {
		rt_printf("error setting up Fft\n");
		return false;
	}
	for(int n = 0; n < kFftWindow; n++) {
		gWindowCoeff[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (kFftWindow - 1)));
	}

	gAnalysisTask = Bela_createAuxiliaryTask(&analyseFrame, 85, "gen1-multicell-analysis");
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

	Bela_getDefaultWatcherManager()->getGui().setup(context->projectName);
	Bela_getDefaultWatcherManager()->setup(context->audioSampleRate);

	gWatchBypass = 0u;   // default: engaged. Host sets this to 1 for the disengaged
	                     // side of an A/B, or leaves it at 0 for normal operation.
	// localControl(false) is what actually makes this settable remotely --
	// without it .get() always returns the locally-assigned value above, never
	// a value a "set" command writes. Found and fixed 2026-09-14 in
	// gen1-multicell-live; ported back here for the same reason.
	gWatchBypass.localControl(false);
	gWatchSweepActive = 0u;

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

	rt_printf("gen1-multicell (N=%d)\n", kNumCells);
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  ceiling: %.3f linear   watchdog: %.0f s   fade-in: %.2f s\n",
	          kOutputCeiling, kWatchdogTimeoutS, kFadeInS);
	rt_printf("  analysis: window=%d hop=%d (%.1f ms), bind-by-magnitude, "
	          "steal-least-active when full\n",
	          kFftWindow, kHop, 1000.0f * kHop / context->audioSampleRate);
	rt_printf("  cell: target=%.1f dB  Q=%.1f  attack=%.1f ms  release=%.0f ms  "
	          "maxCut=%.1f dB  freqLag=%.0f ms\n",
	          kTargetDb, kCellQ, kAttackMs, kReleaseMs, kMaxCutDb, 1000.0f * kFreqLagS);
	rt_printf("  recording: %s (in), %s (out)\n",
	          kInputsFilename.c_str(), kOutputsFilename.c_str());
	rt_printf("  sweep-kick: %.0f-%.0f Hz over %.1f s at %.3f linear, right after fade-in\n",
	          kSweepStartHz, kSweepEndHz, kSweepDurationS, kSweepAmpLinear);
	rt_printf("  bypass: %u (Watcher-settable; 1 = cells computed but not applied to "
	          "the audio path -- for an engaged/disengaged A/B)\n", gWatchBypass.get());

	return true;
}

// ------------------------------------------------------------------ render

void render(BelaContext *context, void *userData)
{
	const float elapsedS = (float)gFrameCount / context->audioSampleRate;

	if(!gMuted && elapsedS >= kWatchdogTimeoutS) {
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
	bool sweepActiveThisBlock = false;
	const bool bypass = (gWatchBypass.get() != 0u);

	// gCellState doesn't change within one render() call (only the aux task
	// writes it, between calls) -- safe to compute once and republish every
	// sample below.
	unsigned int boundCount = 0;
	for(int c = 0; c < kNumCells; c++) {
		if(gCellState[c] == kBound) boundCount++;
	}

	const float sweepStartS = kFadeInS;
	const float sweepEndS = kFadeInS + kSweepDurationS;

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		const uint64_t frames = context->audioFramesElapsed + n;
		Bela_getDefaultWatcherManager()->tick(frames);
		const float sampleElapsedS = (float)(gFrameCount + n) / context->audioSampleRate;

		for(unsigned int ch = 0; ch < context->audioInChannels; ch++) {
			gInputBuf[n * context->audioInChannels + ch] = audioRead(context, n, ch);
		}
		const float in = audioRead(context, n, kGuitarInputChannel);
		const float a = std::fabs(in);
		if(a > inPeak) inPeak = a;

		// Sweep-kick: see the constants' header comment. Added to the excitation
		// signal used for everything downstream (analysis, cells, output) -- NOT
		// to gInputBuf, which stays the true raw pickup signal.
		float sweepSample = 0.0f;
		if(sampleElapsedS >= sweepStartS && sampleElapsedS < sweepEndS) {
			const float tSweep = sampleElapsedS - sweepStartS;
			const float sweepFreq = kSweepStartHz
			                       + (kSweepEndHz - kSweepStartHz) * (tSweep / kSweepDurationS);
			gSweepPhase += 2.0 * M_PI * (double)sweepFreq / (double)context->audioSampleRate;
			if(gSweepPhase > 2.0 * M_PI) gSweepPhase -= 2.0 * M_PI;

			float ramp = 1.0f;
			if(tSweep < kSweepRampS) {
				ramp = 0.5f * (1.0f - cosf((float)M_PI * tSweep / kSweepRampS));
			} else if(tSweep > kSweepDurationS - kSweepRampS) {
				ramp = 0.5f * (1.0f - cosf((float)M_PI * (kSweepDurationS - tSweep) / kSweepRampS));
			}
			sweepSample = kSweepAmpLinear * ramp * sinf((float)gSweepPhase);
			sweepActiveThisBlock = true;
		}
		const float excitation = in + sweepSample;

		gRing[gRingWritePos] = excitation;
		gRingWritePos++;
		if(gRingWritePos >= kRingSize) gRingWritePos = 0;
		gSamplesSinceHop++;
		if(gSamplesSinceHop >= kHop) {
			gSamplesSinceHop = 0;
			Bela_scheduleAuxiliaryTask(gAnalysisTask);
		}

		float sig = excitation;
		for(int c = 0; c < kNumCells; c++) {
			if(gCellState[c] == kBound) {
				gFreqSlewed[c] += (gCandidateFreqHz[c] - gFreqSlewed[c]) * gFreqLagCoeff;
			}
			const float freqNow = clampf(gFreqSlewed[c], kMinCellFreqHz, kMaxCellFreqHz);

			// Rebind duck -- see kRebindDuckS's header comment. A new epoch (fresh
			// bind or steal) restarts the duck at 0 regardless of where it was;
			// otherwise it eases back up to 1.
			if(gLastSeenRebindEpoch[c] != gRebindEpoch[c]) {
				gLastSeenRebindEpoch[c] = gRebindEpoch[c];
				gCellDuck[c] = 0.0f;
			} else if(gCellDuck[c] < 1.0f) {
				gCellDuck[c] = std::min(1.0f, gCellDuck[c] + gDuckIncrement);
			}

			float envLin;
			if(gCellState[c] == kBound) {
				gDetectBpf[c].setFc(freqNow);
				const float partial = (float)gDetectBpf[c].process(excitation);
				envLin = gEnvelope[c].process(partial);
			} else {
				envLin = gEnvelope[c].process(0.0f);
			}
			const float partialDb = 20.0f * log10f(std::max(envLin, 1e-9f));
			const float cutDb = clampf(partialDb - kTargetDb, 0.0f, kMaxCutDb);
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
		// mirroring sc/gen1_cell.scd's Select.ar(bypass, [wet, in]).
		const float selected = bypass ? excitation : sig;

		float out = 0.0f;
		if(!gMuted) {
			if(!std::isfinite(in) || !std::isfinite(excitation) || nonFiniteThisBlock) {
				gMuted = true;
				rt_printf("non-finite sample — output muted\n");
			} else {
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
		gWatchSweepActive = sweepActiveThisBlock ? 1u : 0u;
		gWatchCellsBoundCount = boundCount;
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
