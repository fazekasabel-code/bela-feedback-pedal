/*
 * gen1-cell — Phase 4 of docs/phase-plan.md: one adaptive gain-regulation cell,
 * ported to C++ for Bela from sc/gen1_cell.scd (the old Mac-rig SuperCollider
 * prototype). Same physics, same actuator shape, same safety net, mute-on-error
 * and recording as gen1-passthrough/detector-passthrough -- see those projects'
 * header comments for the rationale, unchanged here.
 *
 *     guitar -> Bela ch0 (direct/unbuffered) -> [ cell ] -> Bela ch1 -> DTA120 -> exciter
 *                                                   ^
 *                          in -> [ STFT growth-detector, aux task ] -> allocator (N=1)
 *
 * The test this exists to answer (phase-plan.md Phase 4) is specific and
 * audible: hold the dominant partial at its target, and a SECOND partial
 * should appear on its own. That is the inhomogeneous-saturation argument of
 * ground-rules-and-facts.md 5, working or not.
 *
 * WHAT THE ACTUATOR IS. Not a fixed-depth notch. A peaking-EQ biquad (negative
 * gain) whose depth is driven by an envelope follower comparing the bound
 * partial's level to a target -- exactly as much reduction as it takes to
 * hold the target, no more. "Suppression" in this project always means
 * regulation to a level, never removal (ground-rules 6.3, CLAUDE.md vocabulary).
 *
 * WHICH PARTIAL THE CELL BINDS TO -- deliberate divergence from sc/gen1_cell.scd.
 * The SC patch used SuperCollider's Pitch.kr (autocorrelation) to find "the"
 * pitch in the signal. This port instead reuses bela/detector-passthrough's
 * STFT (2048/256, Hann, same growth/prominence machinery), already built and
 * hardware-validated in Phase 3, and treats this as the N=1 case of the
 * allocator in ground-rules 6.2 -- a real per-bin candidate pool, quadratic
 * peak interpolation for sub-bin frequency (6.1), one BOUND/FREE cell.
 *
 * BINDING IS BY MAGNITUDE, NOT BY GROWTH RATE -- also deliberate, and worth
 * being explicit about because 6.2 says "arm on growth, not on rank". Growth-
 * rate arming is how detector-passthrough decides a bin is interesting, and
 * phase-plan.md's 2026-09-13 status block logs that its threshold
 * (kArmGrowthDbPerFrame, tuned for the old ~100-1000 dB/s jump estimate) is
 * miscalibrated for this rig's actual measured near-unity growth (~1-2.5 dB/s
 * on a clean single-partial jump) -- it would essentially never fire for the
 * ordinary case this cell exists to regulate. That miscalibration is flagged
 * there as "not blocking Phase 4", which only holds if Phase 4 does not
 * depend on it. So: this cell binds to whichever candidate bin is the
 * loudest *stable* prominent peak above the noise floor (persistence-gated,
 * same anti-noise logic as detector-passthrough) -- a rank-based allocator,
 * adequate for the one-cell "does regulation let a second partial bloom"
 * test, since there is no second cell to mis-steal for. Growth rate is still
 * computed and exposed over Watcher for comparison, just not used to gate
 * binding. Fast jump pre-emption via growth-rate arming is Phase 5's proper
 * multi-cell allocator's job, once that threshold has been retuned.
 *
 * OTHER DELIBERATE SIMPLIFICATION FROM THE SC PATCH: SC cascades two smoothing
 * stages -- Amplitude.kr(attack, release) to get the partial's level, then a
 * second LagUD.kr(excessDb, attack, release) on the excess before it drives
 * the actuator. Both stages use the *same* attack/release times, which mostly
 * just adds latency without adding a musically distinct behaviour. This port
 * uses one EnvelopeDetector (fast attack / slow release) directly on the
 * bound partial's level and derives the cut from its output -- one fewer
 * cascaded filter, same "at or below target the cell does nothing" invariant,
 * less added attack latency (helps, not hurts, the loop-gain safety margin).
 *
 * COEFFICIENT INTERPOLATION: recomputed every audio sample (Bela's Biquad
 * recalculates on every setFc/setPeakGain call), which is the finer of the
 * two options ground-rules 6.3 allows ("per-block, or per-sample-crossfaded").
 * Fine for one cell; worth profiling before Phase 5 multiplies the cell count
 * (phase-plan.md already flags CPU load as a thing to watch from Phase 5 on).
 *
 * SAFETY: kOutputCeiling and kWatchdogTimeoutS are safety constants in the
 * CLAUDE.md sense -- unchanged in meaning and value from gen1-passthrough /
 * detector-passthrough. No automatic tuning process may raise or extend them
 * (ground rules 1, 3). Everything under "cell tuning constants" below is a
 * musical/DSP parameter, not a safety constant -- retune freely, one variable
 * at a time (CLAUDE.md rule 10), each retune a separate committed run.
 *
 * NOT YET RUN AGAINST THE REAL EXCITER as of this commit -- compiled against
 * this board's toolchain only (ground rule 2: real-loop tests need Abel
 * present and the rig confirmed live).
 */

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

Watcher<float>        gWatchInPeak("in_peak");
Watcher<float>        gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");
Watcher<unsigned int> gWatchCellBound("cell_bound");
Watcher<float>        gWatchCellFreqHz("cell_freq_hz");         // slewed, in-use frequency
Watcher<float>        gWatchCellRawFreqHz("cell_raw_freq_hz");  // aux-task candidate, pre-slew
Watcher<float>        gWatchPartialDb("partial_db");
Watcher<float>        gWatchCutDb("cut_db");
Watcher<unsigned int> gWatchBindEvents("bind_events_total");    // monotonic counter
Watcher<unsigned int> gWatchReleaseEvents("release_events_total");

// ---------------------------------------------------------------- constants

// Safety constants -- see header comment. Identical meaning and value to
// gen1-passthrough / detector-passthrough / feedback-ramp.
static const float kOutputCeiling = 0.5f;
static const float kWatchdogTimeoutS = 120.0f;
static const float kFadeInS = 0.2f;

// The guitar is mono and always on this input channel (rig-profile.json
// host.guitar_input_index, confirmed 2026-09-13). Both output channels
// carry the cell's output -- ch1 is what actually feeds the DTA120/exciter
// (host.exciter_output_index), ch0 is unused downstream but written too for
// symmetry with every other project in this repo.
static const unsigned int kGuitarInputChannel = 0;

// ---- analysis (STFT growth/prominence detector, reused from
// bela/detector-passthrough -- see that project's header for the fuller
// rationale, unchanged here except where noted above).
static const int   kFftWindow = 2048;
static const int   kHop = 256;
static const int   kNumBins = kFftWindow / 2 + 1;
static const int   kRingSize = kFftWindow * 4;
static const float kGrowthSmoothing = 0.6f;      // telemetry only in this port -- see
                                                  // header note on why binding does not
                                                  // gate on growth rate here.
static const int   kStableHoldFrames = 2;        // frames a bin must be a prominent peak
                                                  // before it is eligible to bind (same
                                                  // role as detector-passthrough's
                                                  // kArmHoldFrames, renamed since it no
                                                  // longer requires growth to also
                                                  // qualify).
static const float kReleaseMarginDb = 10.0f;
static const int   kReleaseHoldFrames = 8;
static const float kSilenceDbfs = -80.0f;
static const float kProminenceDb = 25.0f;        // see detector-passthrough's header
                                                  // comment for the full story: 25 dB
                                                  // reproduced zero false arms against a
                                                  // real quiet-room recording where the
                                                  // Python reference's 6 dB false-armed
                                                  // 150-230 of 1025 bins continuously.
static const int   kNeighborOffsetBins = 4;      // cheap local-shoulder prominence proxy
static const int   kGlideBinRadius = 2;          // once bound, follow the local peak if
                                                  // it moves at most this many bins --
                                                  // ground-rules 6.2's "~50 cents" glide
                                                  // window, expressed in bins rather than
                                                  // cents since 50 cents is sub-bin at
                                                  // guitar-partial frequencies with this
                                                  // window/rate anyway (quadratic
                                                  // interpolation below is what actually
                                                  // buys the sub-bin accuracy).
static const int   kMinBoundHoldFrames = 40;     // anti-chatter: ignore release
                                                  // conditions for this many frames right
                                                  // after binding (~232 ms at hop 256 /
                                                  // 44.1 kHz).

// ---- cell / actuator tuning (ground-rules 6.3; values carried over from
// sc/gen1_cell.scd's defaults, the one working reference this is ported from).
static const float kTargetDb = -24.0f;    // level this partial is allowed to reach
static const float kCellQ = 10.0f;        // SC's rq=0.1 -> Q=10 (ground-rules 6.3: 8-20)
static const float kAttackMs = 3.0f;      // must beat the loop's growth rate (6.3: 1-5ms)
static const float kReleaseMs = 400.0f;   // musical parameter -- how long a partial
                                           // stays "used up" (6.3: 100ms-2s)
static const float kMaxCutDb = 30.0f;     // the cell may never pull harder than this
static const float kFreqLagS = 0.02f;     // frequency slew time constant -- not zero, or
                                           // coefficient jumps click (SC's freqLag)
static const float kMinCellFreqHz = 55.0f;
static const float kMaxCellFreqHz = 2000.0f;

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

// Analysis ring buffer -- same structure as detector-passthrough. Fed with the
// raw input (pre-actuator), matching sc/gen1_cell.scd's Pitch.kr(in): what to
// regulate is decided from the incoming loop signal, not our own output.
static std::vector<float> gRing(kRingSize, 0.0f);
static int gRingWritePos = 0;
static int gSamplesSinceHop = 0;

static Fft gFft;
static std::vector<float> gWindowCoeff(kFftWindow);
static std::vector<float> gAnalysisFrame(kFftWindow);
static std::vector<float> gPrevMagDb(kNumBins, -200.0f);
static std::vector<float> gSmoothedGrowth(kNumBins, 0.0f);   // telemetry only, see header
static std::vector<int>   gPersistence(kNumBins, 0);

static float gSampleRate = 44100.0f;   // real value set in setup(); the auxiliary task
                                        // callback has no BelaContext of its own.

static AuxiliaryTask gAnalysisTask;

// ---- allocator state (N=1 cell). Written only by the auxiliary task, read
// only by the audio thread -- same single-writer/single-reader convention
// bela/detector-passthrough already relies on for its armed-bin arrays.
enum CellState { kFree = 0, kBound = 1 };
static CellState gCellState = kFree;
static int   gBoundBin = -1;
static int   gBoundFrames = 0;
static float gBoundPeakMagDb = -200.0f;
static int   gBelowReleaseFrames = 0;
static float gCandidateFreqHz = 220.0f;   // quadratic-interpolated frequency of the
                                           // bound bin (or last bound bin) -- 220 Hz
                                           // default matches SC's Pitch.kr initFreq.
static unsigned int gBindEventsTotal = 0;
static unsigned int gReleaseEventsTotal = 0;

// ---- audio-rate cell DSP
static float gFreqLagCoeff = 0.0f;   // set from kFreqLagS and the real sample rate
static float gFreqSlewed = 220.0f;   // one-pole-slewed frequency actually fed to the
                                      // filters below; frozen while the cell is FREE
                                      // (harmless -- cut has already decayed to ~0 dB
                                      // by the time release completes).
static Biquad gDetectBpf;            // bandpass, tracks the bound partial's level
static Biquad gActuatorEq;           // peaking EQ, negative gain = the actual cut
static EnvelopeDetector gEnvelope;   // fast attack / slow release, drives the cut

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

// Cheap local-shoulder "is this bin a prominent peak" test -- identical to
// bela/detector-passthrough's isProminentPeak(). See that project's header
// comment for why this is not scipy.signal.find_peaks' true prominence walk,
// and why kProminenceDb is 25 dB here rather than the Python reference's 6 dB.
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

// Quadratic (parabolic) peak interpolation for sub-bin frequency accuracy
// (ground-rules 6.1). Bin i must not be at the very edge of the spectrum --
// isProminentPeak()'s own bounds check already guarantees that for any bin
// this is called on.
static float interpolatedBinToHz(const std::vector<float>& magDb, int i)
{
	const float alpha = magDb[i - 1];
	const float beta  = magDb[i];
	const float gamma = magDb[i + 1];
	const float denom = (alpha - 2.0f * beta + gamma);
	float p = 0.0f;
	if(std::fabs(denom) > 1e-9f) {
		p = 0.5f * (alpha - gamma) / denom;
		p = clampf(p, -0.5f, 0.5f);   // a real peak's vertex never lands outside
		                              // the bin either side of it; guards a stray
		                              // near-zero denominator from blowing up.
	}
	return (i + p) * gSampleRate / (float)kFftWindow;
}

// The auxiliary task: STFT + the N=1 allocator's bind/glide/release logic.
// Lower priority than the audio thread, scheduled once per hop -- never on
// the audio thread itself (ground-rules 6.1: "STFT on an auxiliary task so it
// can never miss the audio deadline").
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

	int bestFreeBin = -1;
	float bestFreeMagDb = -200.0f;

	for(int i = 0; i < kNumBins; i++) {
		// Telemetry only -- see header comment on why binding does not gate on this.
		const float growth = magDb[i] - gPrevMagDb[i];
		gSmoothedGrowth[i] = kGrowthSmoothing * growth + (1.0f - kGrowthSmoothing) * gSmoothedGrowth[i];

		const bool isPeak = isProminentPeak(magDb, i);
		gPersistence[i] = isPeak ? gPersistence[i] + 1 : 0;

		if(gCellState == kFree
		   && isPeak && magDb[i] > kSilenceDbfs && gPersistence[i] >= kStableHoldFrames
		   && magDb[i] > bestFreeMagDb) {
			bestFreeBin = i;
			bestFreeMagDb = magDb[i];
		}

		gPrevMagDb[i] = magDb[i];
	}

	if(gCellState == kFree) {
		if(bestFreeBin >= 0) {
			gCellState = kBound;
			gBoundBin = bestFreeBin;
			gBoundFrames = 0;
			gBoundPeakMagDb = bestFreeMagDb;
			gBelowReleaseFrames = 0;
			gBindEventsTotal++;
			gCandidateFreqHz = interpolatedBinToHz(magDb, gBoundBin);
		}
		// else: stay FREE, gCandidateFreqHz untouched (frozen at its last value --
		// harmless, see gFreqSlewed's comment).
	} else {
		gBoundFrames++;

		// Glide: follow the local peak within kGlideBinRadius bins either side of
		// where the cell is currently bound (ground-rules 6.2's glide window).
		int newBin = gBoundBin;
		float newBinMagDb = magDb[gBoundBin];
		const int lo = std::max(kNeighborOffsetBins * 2, gBoundBin - kGlideBinRadius);
		const int hi = std::min(kNumBins - kNeighborOffsetBins * 2 - 1, gBoundBin + kGlideBinRadius);
		for(int i = lo; i <= hi; i++) {
			if(magDb[i] > newBinMagDb) {
				newBin = i;
				newBinMagDb = magDb[i];
			}
		}
		gBoundBin = newBin;
		gCandidateFreqHz = interpolatedBinToHz(magDb, gBoundBin);
		gBoundPeakMagDb = std::max(gBoundPeakMagDb, newBinMagDb);

		if(gBoundFrames > kMinBoundHoldFrames) {
			if(newBinMagDb <= gBoundPeakMagDb - kReleaseMarginDb) {
				gBelowReleaseFrames++;
			} else {
				gBelowReleaseFrames = 0;
			}
			if(gBelowReleaseFrames >= kReleaseHoldFrames) {
				gCellState = kFree;
				gBoundBin = -1;
				gReleaseEventsTotal++;
			}
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

	gCellState = kFree;
	gBoundBin = -1;
	gBoundFrames = 0;
	gBoundPeakMagDb = -200.0f;
	gBelowReleaseFrames = 0;
	gCandidateFreqHz = 220.0f;
	gBindEventsTotal = 0;
	gReleaseEventsTotal = 0;

	gFreqSlewed = 220.0f;
	gFreqLagCoeff = 1.0f - expf(-1.0f / (kFreqLagS * gSampleRate));

	if(gFft.setup(kFftWindow)) {
		rt_printf("error setting up Fft\n");
		return false;
	}
	for(int n = 0; n < kFftWindow; n++) {
		gWindowCoeff[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (kFftWindow - 1)));
	}

	gAnalysisTask = Bela_createAuxiliaryTask(&analyseFrame, 85, "gen1-cell-analysis");
	if(!gAnalysisTask) {
		rt_printf("error creating the analysis auxiliary task\n");
		return false;
	}

	BiquadCoeff::Settings bpfSettings {
		.fs = (double)gSampleRate, .type = BiquadCoeff::bandpass,
		.cutoff = gFreqSlewed, .q = kCellQ, .peakGainDb = 0.0
	};
	gDetectBpf.setup(bpfSettings);

	BiquadCoeff::Settings eqSettings {
		.fs = (double)gSampleRate, .type = BiquadCoeff::peak,
		.cutoff = gFreqSlewed, .q = kCellQ, .peakGainDb = 0.0
	};
	gActuatorEq.setup(eqSettings);

	gEnvelope.setup(kAttackMs, kReleaseMs, gSampleRate,
	                EnvelopeDetector::ANALOG, EnvelopeDetector::BRANCHING,
	                EnvelopeDetector::PEAK, true);

	Bela_getDefaultWatcherManager()->getGui().setup(context->projectName);
	Bela_getDefaultWatcherManager()->setup(context->audioSampleRate);

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

	rt_printf("gen1-cell\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  ceiling: %.3f linear   watchdog: %.0f s   fade-in: %.2f s\n",
	          kOutputCeiling, kWatchdogTimeoutS, kFadeInS);
	rt_printf("  analysis: window=%d hop=%d (%.1f ms), bind-by-magnitude "
	          "(see header comment on why not growth-rate)\n",
	          kFftWindow, kHop, 1000.0f * kHop / context->audioSampleRate);
	rt_printf("  cell: target=%.1f dB  Q=%.1f  attack=%.1f ms  release=%.0f ms  "
	          "maxCut=%.1f dB  freqLag=%.0f ms\n",
	          kTargetDb, kCellQ, kAttackMs, kReleaseMs, kMaxCutDb, 1000.0f * kFreqLagS);
	rt_printf("  recording: %s (in), %s (out)\n",
	          kInputsFilename.c_str(), kOutputsFilename.c_str());

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
	float lastPartialDb = -200.0f, lastCutDb = 0.0f;

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		const uint64_t frames = context->audioFramesElapsed + n;
		Bela_getDefaultWatcherManager()->tick(frames);

		for(unsigned int ch = 0; ch < context->audioInChannels; ch++) {
			gInputBuf[n * context->audioInChannels + ch] = audioRead(context, n, ch);
		}
		// Mono guitar, always this one input channel -- see header comment.
		const float in = audioRead(context, n, kGuitarInputChannel);
		const float a = std::fabs(in);
		if(a > inPeak) inPeak = a;

		// Feed the analysis ring buffer with the raw (pre-actuator) signal and
		// schedule the auxiliary task every kHop samples.
		gRing[gRingWritePos] = in;
		gRingWritePos++;
		if(gRingWritePos >= kRingSize) gRingWritePos = 0;
		gSamplesSinceHop++;
		if(gSamplesSinceHop >= kHop) {
			gSamplesSinceHop = 0;
			Bela_scheduleAuxiliaryTask(gAnalysisTask);
		}

		// Frequency slew (SC's Lag.kr(freqLag)) -- only chases the aux task's
		// candidate while bound; frozen while FREE, matching SC's
		// Lag.kr(Gate.kr(rawFreq, hasFreq)) holding its last value when the
		// pitch tracker is unsure instead of chasing an unrelated frequency.
		if(gCellState == kBound) {
			gFreqSlewed += (gCandidateFreqHz - gFreqSlewed) * gFreqLagCoeff;
		}
		const float freqNow = clampf(gFreqSlewed, kMinCellFreqHz, kMaxCellFreqHz);

		// Envelope follower on the bound partial's level. While FREE, feed the
		// envelope silence so the cut ramps back over the release time instead
		// of resetting instantly (ground-rules 6.3: "on release, ramp back") --
		// it never runs the detection filter on a stale/meaningless frequency.
		float envLin;
		if(gCellState == kBound) {
			gDetectBpf.setFc(freqNow);
			const float partial = (float)gDetectBpf.process(in);
			envLin = gEnvelope.process(partial);
		} else {
			envLin = gEnvelope.process(0.0f);
		}
		const float partialDb = 20.0f * log10f(std::max(envLin, 1e-9f));
		const float cutDb = clampf(partialDb - kTargetDb, 0.0f, kMaxCutDb);

		gActuatorEq.setFc(freqNow);
		gActuatorEq.setPeakGain(-cutDb);
		const float actuatorOut = (float)gActuatorEq.process(in);

		float out = 0.0f;
		if(!gMuted) {
			if(!std::isfinite(in) || !std::isfinite(actuatorOut)
			   || !std::isfinite(freqNow) || !std::isfinite(cutDb)) {
				gMuted = true;
				rt_printf("non-finite sample — output muted\n");
			} else {
				out = clampToCeiling(actuatorOut) * fadeGain;
			}
		}

		for(unsigned int ch = 0; ch < context->audioOutChannels; ch++) {
			audioWrite(context, n, ch, out);
			gOutputBuf[n * context->audioOutChannels + ch] = out;
		}
		const float ao = std::fabs(out);
		if(ao > outPeak) outPeak = ao;

		lastPartialDb = partialDb;
		lastCutDb = cutDb;
	}

	gInputWriter.setSamples(gInputBuf);
	gOutputWriter.setSamples(gOutputBuf);

	gWatchInPeak = inPeak;
	gWatchOutPeak = outPeak;
	gWatchMuted = gMuted ? 1u : 0u;
	gWatchCellBound = (gCellState == kBound) ? 1u : 0u;
	gWatchCellFreqHz = gFreqSlewed;
	gWatchCellRawFreqHz = gCandidateFreqHz;
	gWatchPartialDb = lastPartialDb;
	gWatchCutDb = lastCutDb;
	gWatchBindEvents = gBindEventsTotal;
	gWatchReleaseEvents = gReleaseEventsTotal;

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
