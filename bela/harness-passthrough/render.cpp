/*
 * harness-passthrough — Phase 1 host-harness Bela project for bela-feedback-pedal.
 *
 * Same passthrough behaviour and safety net as bela/gen1-passthrough/ (hard output
 * ceiling, mute-on-non-finite, watchdog, startup fade-in) -- see that file's header
 * comment for the full rationale, unchanged here. This project adds only what
 * Phase 1's host harness needs on top:
 *
 *   - records input and output audio to inputs.wav / outputs.wav on the SD card via
 *     Bela's AudioFileWriter (libraries/AudioFile), so a take can be fetched and
 *     scored by host/harness/metrics.py after the run. Input is recorded raw,
 *     pre-safety-processing; output is recorded post (ceiling, mute, fade-in) --
 *     i.e. what actually left the board and could reach the exciter, which is what
 *     the proxy metrics in ground-rules-and-facts.md 9 are meant to score.
 *   - streams input/output peak level and mute state over Watcher/pybela
 *     (Watcher.h/.cpp vendored here exactly as in bela/watcher-check/, see that
 *     project's header comment for the compile fixes this needed) for optional
 *     live monitoring during a run -- not required for a take to be scoreable,
 *     the recording is what host/harness/metrics.py actually reads.
 *
 * This is what host/rig/harness_run.py deploys, runs for a fixed duration, and
 * fetches recordings from. See that script and docs/phase-plan.md Phase 1.
 *
 * SAFETY: kOutputCeiling and kWatchdogTimeoutS are safety constants, unchanged in
 * meaning from gen1-passthrough -- no automatic tuning may raise/extend them, they
 * change only by an explicit human commit. See CLAUDE.md rules 1 and 3.
 */

#include <array>
#include <Bela.h>
#include <Watcher.h>
#include <libraries/AudioFile/AudioFile.h>
#include <cmath>
#include <string>
#include <vector>

Watcher<float> gWatchInPeak("in_peak");
Watcher<float> gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");

// ---------------------------------------------------------------- constants

static const float kOutputCeiling = 0.5f;      // SAFETY CONSTANT -- see header comment.
static const float kWatchdogTimeoutS = 120.0f; // SAFETY CONSTANT -- see header comment.
static const float kFadeInS = 0.2f;            // device protection, see gen1-passthrough.

static const std::string kInputsFilename = "inputs.wav";
static const std::string kOutputsFilename = "outputs.wav";
static const size_t kAudioFileBufferSize = 16384;

// ------------------------------------------------------------------- state

static bool     gMuted = false;
static uint64_t gFrameCount = 0;

static AudioFileWriter gInputWriter;
static AudioFileWriter gOutputWriter;
static std::vector<float> gInputBuf;   // interleaved, raw input, per block
static std::vector<float> gOutputBuf;  // interleaved, post-safety output, per block

// ------------------------------------------------------------------ helpers

static inline float clampToCeiling(float x)
{
	if(x >  kOutputCeiling) return  kOutputCeiling;
	if(x < -kOutputCeiling) return -kOutputCeiling;
	return x;
}

// ------------------------------------------------------------------- setup

bool setup(BelaContext *context, void *userData)
{
	gMuted = false;
	gFrameCount = 0;

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

	rt_printf("harness-passthrough\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  ceiling: %.3f linear\n", kOutputCeiling);
	rt_printf("  watchdog: mutes at %.0f s\n", kWatchdogTimeoutS);
	rt_printf("  fade-in: %.2f s\n", kFadeInS);
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

		for(unsigned int ch = 0; ch < context->audioInChannels; ch++) {
			const float in = audioRead(context, n, ch);
			gInputBuf[n * context->audioInChannels + ch] = in;
			const float a = std::fabs(in);
			if(a > inPeak) inPeak = a;
		}

		for(unsigned int ch = 0; ch < context->audioOutChannels; ch++) {
			float out = 0.0f;

			if(!gMuted) {
				const unsigned int inCh = (ch < context->audioInChannels) ? ch : 0;
				const float in = audioRead(context, n, inCh);

				// Fail safe: any non-finite sample latches mute for the rest of
				// the run. Do not try to recover and keep going.
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

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
