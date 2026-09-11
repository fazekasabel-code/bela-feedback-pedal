/*
 * detector-passthrough — Phase 3 analysis layer for bela-feedback-pedal.
 *
 * "Gain cells present but at unity — the signal passes through untouched"
 * (docs/phase-plan.md Phase 3). Same passthrough behaviour, safety net and
 * recording as bela/harness-passthrough/ (output ceiling, mute-on-non-finite,
 * watchdog, startup fade-in, inputs.wav/outputs.wav) -- see that project's
 * header comment for the rationale, unchanged here. What this project adds is
 * the real-time port of host/harness/detector.py's growth-rate arm/release
 * detector (ground-rules-and-facts.md 6.1 / 7):
 *
 *   - STFT on an AuxiliaryTask (2048-window, 256-hop Hann, matching the
 *     Python reference exactly), so analysis can never miss the audio
 *     deadline -- it runs at lower priority on its own thread.
 *   - Per-bin growth rate (one-pole smoothed dB/frame), a peak/persistence
 *     gate, and arm/release state, identical in structure to
 *     GrowthDetector in host/harness/detector.py.
 *   - Exposes the count of currently-armed bins and the most recent arm
 *     event's frequency/level/growth over Watcher/pybela.
 *
 * DIVERGENCE FROM THE PYTHON REFERENCE, both deliberate: peak/prominence is
 * a cheap local-shoulder comparison here (average of two fixed-offset
 * neighbour pairs), not scipy.signal.find_peaks' true prominence walk --
 * real-time-safe and allocation-free, at the cost of being an approximation
 * that needs its own, higher threshold (kProminenceDb's comment has the
 * story: 6 dB, the Python module's value, false-armed 150-230 of 1025 bins
 * continuously on real recorded room noise; 25 dB does not, checked against
 * the same recording). And there is no glide/allocator layer (ground-rules
 * 6.2) -- persistence and arming are tracked per fixed FFT bin, not per
 * drifting partial. Both match Phase 3's actual scope: the allocator is
 * Phase 5's job.
 *
 * SAFETY: kOutputCeiling and kWatchdogTimeoutS are safety constants, meaning
 * unchanged from gen1-passthrough/harness-passthrough -- no automatic tuning
 * may raise/extend them. See CLAUDE.md rules 1 and 3. Everything under
 * "detector constants" below is a DSP tuning parameter, not a safety
 * constant -- it can be retuned freely, it just isn't safety-critical.
 */

#include <array>
#include <Bela.h>
#include <Watcher.h>
#include <libraries/Fft/Fft.h>
#include <libraries/AudioFile/AudioFile.h>
#include <cmath>
#include <string>
#include <vector>

Watcher<float>        gWatchInPeak("in_peak");
Watcher<float>        gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");
Watcher<unsigned int> gWatchArmedCount("armed_count");
Watcher<float>        gWatchLastArmFreq("last_arm_freq_hz");
Watcher<float>        gWatchLastArmMagDb("last_arm_magnitude_db");
Watcher<float>        gWatchLastArmGrowth("last_arm_growth_db_per_frame");
Watcher<unsigned int> gWatchArmEvents("arm_events_total");    // monotonic counter
Watcher<unsigned int> gWatchReleaseEvents("release_events_total");

// ---------------------------------------------------------------- constants

// Safety constants -- see header comment. Identical meaning to gen1-passthrough
// and harness-passthrough.
static const float kOutputCeiling = 0.5f;
static const float kWatchdogTimeoutS = 120.0f;
static const float kFadeInS = 0.2f;

// Detector constants -- DSP tuning, mirrors host/harness/detector.py's defaults.
static const int   kFftWindow = 2048;
static const int   kHop = 256;
static const int   kNumBins = kFftWindow / 2 + 1;
static const int   kRingSize = kFftWindow * 4;   // ample margin over the window,
                                                  // same idea as Bela's own
                                                  // FFT-phase-vocoder example
static const float kGrowthSmoothing = 0.6f;
static const float kArmGrowthDbPerFrame = 0.8f;  // ~140 dB/s at 44.1 kHz/hop 256 --
                                                  // see detector.py's comment on this
                                                  // same constant for the reasoning
static const int   kArmHoldFrames = 2;
static const float kReleaseMarginDb = 10.0f;
static const int   kReleaseHoldFrames = 8;
static const float kSilenceDbfs = -80.0f;
static const float kProminenceDb = 25.0f;        // NOT the same number as detector.py's
                                                  // PROMINENCE_DB=6 -- that guards a true
                                                  // prominence walk (scipy.find_peaks);
                                                  // this guards the cheap local-shoulder
                                                  // proxy below, which is far more
                                                  // permissive on real broadband signal
                                                  // at the same nominal threshold. 6 dB
                                                  // here false-armed 150-230 of 1025 bins
                                                  // continuously on a real, quiet
                                                  // (-31 dBFS RMS) room-noise recording
                                                  // (logs/2026-09-11/outputs.wav) --
                                                  // found and fixed 2026-09-12 by
                                                  // sweeping this constant offline
                                                  // against that same recording, in
                                                  // Python, before touching hardware
                                                  // again. 25 reproduced zero false arms
                                                  // on it end to end, with no change at
                                                  // all to the synthetic fast-jump
                                                  // detection latency test.
static const int   kNeighborOffsetBins = 4;      // cheap local-shoulder estimate

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

// Analysis: a mono mix of the input channels feeds a circular buffer; every
// kHop samples the audio thread schedules the auxiliary task, which reads the
// last kFftWindow samples out of it. kRingSize >> kFftWindow keeps the aux
// task's read well clear of the audio thread's next write, same margin as
// Bela's own FFT-phase-vocoder example uses for the same reason.
static std::vector<float> gRing(kRingSize, 0.0f);
static int gRingWritePos = 0;
static int gSamplesSinceHop = 0;

static Fft gFft;
static std::vector<float> gWindowCoeff(kFftWindow);
static std::vector<float> gAnalysisFrame(kFftWindow);
static std::vector<float> gPrevMagDb(kNumBins, -200.0f);
static std::vector<float> gSmoothedGrowth(kNumBins, 0.0f);
static std::vector<int>   gPersistence(kNumBins, 0);
static std::vector<int>   gQualifyingFrames(kNumBins, 0);
static std::vector<unsigned char> gArmed(kNumBins, 0);
static std::vector<float> gArmedLevelDb(kNumBins, -200.0f);
static std::vector<int>   gBelowReleaseFrames(kNumBins, 0);

static unsigned int gArmedCount = 0;
static unsigned int gArmEventsTotal = 0;
static unsigned int gReleaseEventsTotal = 0;
static float gLastArmFreqHz = 0.0f;
static float gLastArmMagDb = -200.0f;
static float gLastArmGrowth = 0.0f;
static float gSampleRate = 44100.0f;  // real value set in setup(); the auxiliary
                                       // task callback (void(*)(void*)) has no
                                       // BelaContext of its own, so this is how it
                                       // learns the rate for bin->Hz conversion.

static AuxiliaryTask gAnalysisTask;

// ------------------------------------------------------------------ helpers

static inline float clampToCeiling(float x)
{
	if(x >  kOutputCeiling) return  kOutputCeiling;
	if(x < -kOutputCeiling) return -kOutputCeiling;
	return x;
}

// Cheap local-shoulder "is this bin a prominent peak" test -- see header comment
// on why this is not scipy.signal.find_peaks' true prominence.
static bool isProminentPeak(const std::vector<float>& magDb, int i)
{
	if(i < kNeighborOffsetBins * 2 || i >= kNumBins - kNeighborOffsetBins * 2)
		return false;
	if(magDb[i] < magDb[i - 1] || magDb[i] < magDb[i + 1])
		return false;   // not even a local max against its immediate neighbours

	const float shoulderLeft  = 0.5f * (magDb[i - kNeighborOffsetBins] + magDb[i - kNeighborOffsetBins * 2]);
	const float shoulderRight = 0.5f * (magDb[i + kNeighborOffsetBins] + magDb[i + kNeighborOffsetBins * 2]);
	const float shoulder = std::max(shoulderLeft, shoulderRight);
	return (magDb[i] - shoulder) >= kProminenceDb;
}

// The auxiliary task: runs the STFT and the growth-rate arm/release logic.
// Lower priority than the audio thread, scheduled once per hop -- never on
// the audio thread itself, so it can never cause an xrun no matter how long
// it takes (ground-rules 6.1: "STFT on an auxiliary task so it can never miss
// the audio deadline").
void analyseFrame(void*)
{
	// Copy the last kFftWindow samples out of the ring buffer, applying the
	// analysis window, into a private scratch buffer the FFT can consume.
	int readPos = (gRingWritePos - kFftWindow + kRingSize) % kRingSize;
	for(int n = 0; n < kFftWindow; n++) {
		gAnalysisFrame[n] = gRing[readPos] * gWindowCoeff[n];
		readPos++;
		if(readPos >= kRingSize) readPos = 0;
	}

	gFft.fft(gAnalysisFrame);

	std::array<float, kNumBins> magDbArr{};
	for(int i = 0; i < kNumBins; i++) {
		const float mag = gFft.fda(i) / (kFftWindow / 2.0f);
		magDbArr[i] = 20.0f * log10f(std::max(mag, 1e-12f));
	}
	std::vector<float> magDb(magDbArr.begin(), magDbArr.end());

	for(int i = 0; i < kNumBins; i++) {
		const float growth = magDb[i] - gPrevMagDb[i];
		gSmoothedGrowth[i] = kGrowthSmoothing * growth + (1.0f - kGrowthSmoothing) * gSmoothedGrowth[i];

		const bool isPeak = isProminentPeak(magDb, i);
		gPersistence[i] = isPeak ? gPersistence[i] + 1 : 0;

		const bool qualifies = isPeak && magDb[i] > kSilenceDbfs
		                        && gSmoothedGrowth[i] >= kArmGrowthDbPerFrame;
		gQualifyingFrames[i] = qualifies ? gQualifyingFrames[i] + 1 : 0;

		if(!gArmed[i] && gQualifyingFrames[i] >= kArmHoldFrames) {
			gArmed[i] = 1;
			gArmedLevelDb[i] = magDb[i];
			gBelowReleaseFrames[i] = 0;
			gArmedCount++;
			gArmEventsTotal++;
			gLastArmFreqHz = i * gSampleRate / kFftWindow;
			gLastArmMagDb = magDb[i];
			gLastArmGrowth = gSmoothedGrowth[i];
		} else if(gArmed[i]) {
			gArmedLevelDb[i] = std::max(gArmedLevelDb[i], magDb[i]);
			if(magDb[i] <= gArmedLevelDb[i] - kReleaseMarginDb) {
				gBelowReleaseFrames[i]++;
			} else {
				gBelowReleaseFrames[i] = 0;
			}
			if(gBelowReleaseFrames[i] >= kReleaseHoldFrames) {
				gArmed[i] = 0;
				gQualifyingFrames[i] = 0;
				gArmedCount--;
				gReleaseEventsTotal++;
			}
		}

		gPrevMagDb[i] = magDb[i];
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

	if(gFft.setup(kFftWindow)) {
		rt_printf("error setting up Fft\n");
		return false;
	}
	for(int n = 0; n < kFftWindow; n++) {
		gWindowCoeff[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (kFftWindow - 1)));
	}

	gAnalysisTask = Bela_createAuxiliaryTask(&analyseFrame, 85, "growth-detector");
	if(!gAnalysisTask) {
		rt_printf("error creating the analysis auxiliary task\n");
		return false;
	}

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

	rt_printf("detector-passthrough\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  ceiling: %.3f linear   watchdog: %.0f s   fade-in: %.2f s\n",
	          kOutputCeiling, kWatchdogTimeoutS, kFadeInS);
	rt_printf("  analysis: window=%d hop=%d (%.1f ms) arm-growth=%.2f dB/frame\n",
	          kFftWindow, kHop, 1000.0f * kHop / context->audioSampleRate,
	          kArmGrowthDbPerFrame);
	rt_printf("  gain cells: unity (Phase 3 -- actuator disabled by design)\n");
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

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		const uint64_t frames = context->audioFramesElapsed + n;
		Bela_getDefaultWatcherManager()->tick(frames);

		float mono = 0.0f;
		for(unsigned int ch = 0; ch < context->audioInChannels; ch++) {
			const float in = audioRead(context, n, ch);
			gInputBuf[n * context->audioInChannels + ch] = in;
			mono += in;
			const float a = std::fabs(in);
			if(a > inPeak) inPeak = a;
		}
		if(context->audioInChannels > 0) mono /= context->audioInChannels;

		// Feed the analysis ring buffer and schedule the auxiliary task every
		// kHop samples -- gain cells stay at unity (Phase 3: actuator disabled),
		// this is purely observation.
		gRing[gRingWritePos] = mono;
		gRingWritePos++;
		if(gRingWritePos >= kRingSize) gRingWritePos = 0;
		gSamplesSinceHop++;
		if(gSamplesSinceHop >= kHop) {
			gSamplesSinceHop = 0;
			Bela_scheduleAuxiliaryTask(gAnalysisTask);
		}

		for(unsigned int ch = 0; ch < context->audioOutChannels; ch++) {
			float out = 0.0f;

			if(!gMuted) {
				const unsigned int inCh = (ch < context->audioInChannels) ? ch : 0;
				const float in = audioRead(context, n, inCh);

				if(!std::isfinite(in)) {
					gMuted = true;
					rt_printf("non-finite input sample — output muted\n");
				} else {
					out = clampToCeiling(in) * fadeGain;
				}
			}

			audioWrite(context, n, ch, out);
			gOutputBuf[n * context->audioOutChannels + ch] = out;
			const float a = std::fabs(out);
			if(a > outPeak) outPeak = a;
		}
	}

	gInputWriter.setSamples(gInputBuf);
	gOutputWriter.setSamples(gOutputBuf);

	gWatchInPeak = inPeak;
	gWatchOutPeak = outPeak;
	gWatchMuted = gMuted ? 1u : 0u;
	gWatchArmedCount = gArmedCount;
	gWatchLastArmFreq = gLastArmFreqHz;
	gWatchLastArmMagDb = gLastArmMagDb;
	gWatchLastArmGrowth = gLastArmGrowth;
	gWatchArmEvents = gArmEventsTotal;
	gWatchReleaseEvents = gReleaseEventsTotal;

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
