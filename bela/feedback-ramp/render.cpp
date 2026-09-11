/*
 * feedback-ramp — Phase 1 feedback-threshold measurement for bela-feedback-pedal.
 *
 * There is no volume pedal in this build to serve as the "loop gain" control
 * ground-rules-and-facts.md 8 assumes (see 4.5). This project stands in for it,
 * temporarily and only for this measurement: a software loop-gain multiplier that
 * ramps linearly from 0 to 1 (fully open passthrough) over kRampDurationS seconds,
 * then holds at 1. Everything else is bela/harness-passthrough/ unchanged: same
 * output ceiling, mute-on-non-finite, watchdog, startup fade-in, and
 * inputs.wav/outputs.wav recording -- see that project's header comment for the
 * safety rationale.
 *
 * The point is not to watch it live and react -- it is to let it record, then find
 * exactly when sustained energy starts growing in the recording afterward
 * (host/rig/*, offline) and read the corresponding loop-gain value off the known
 * ramp function g(t) = min(t / kRampDurationS, 1). That is the measured feedback
 * threshold at whatever the Dayton DTA120 is set to right now -- ground-rules 4.2's
 * "Feedback threshold: volume-pedal position at which the loop reaches unity",
 * translated to this build's actual control.
 *
 * Safe by construction regardless of how the ramp behaves: the output ceiling
 * clamps hard no matter what g reaches, and the watchdog caps total run length.
 * Abel present per ground rule 2; no dedicated hardware kill in this build
 * (ground-rules 4.5) -- the DTA120's power switch is the manual backstop.
 *
 * SAFETY: kOutputCeiling and kWatchdogTimeoutS are safety constants, unchanged in
 * meaning from gen1-passthrough/harness-passthrough. kRampDurationS is a
 * measurement parameter, not a safety constant -- it can be changed freely.
 */

#include <array>
#include <Bela.h>
#include <Watcher.h>
#include <libraries/AudioFile/AudioFile.h>
#include <cmath>
#include <string>
#include <vector>

Watcher<float>        gWatchInPeak("in_peak");
Watcher<float>        gWatchOutPeak("out_peak");
Watcher<unsigned int> gWatchMuted("muted");
Watcher<float>        gWatchLoopGain("loop_gain");

// ---------------------------------------------------------------- constants

static const float kOutputCeiling = 0.5f;      // SAFETY CONSTANT -- see header comment.
static const float kWatchdogTimeoutS = 120.0f; // SAFETY CONSTANT -- see header comment.
static const float kFadeInS = 0.2f;            // device protection, see harness-passthrough.

// Measurement parameter, not a safety constant. 60 s leaves a comfortable margin
// under the 120 s watchdog even after reaching full gain and holding there a while.
static const float kRampDurationS = 60.0f;

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

	rt_printf("feedback-ramp\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  ceiling: %.3f linear   watchdog: %.0f s   fade-in: %.2f s\n",
	          kOutputCeiling, kWatchdogTimeoutS, kFadeInS);
	rt_printf("  loop-gain ramp: 0 -> 1 over %.0f s, then holds at 1\n", kRampDurationS);
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

	float loopGain = elapsedS / kRampDurationS;
	if(loopGain > 1.0f) loopGain = 1.0f;
	if(loopGain < 0.0f) loopGain = 0.0f;

	const float totalGain = fadeGain * loopGain;

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

				if(!std::isfinite(in)) {
					gMuted = true;
					rt_printf("non-finite input sample — output muted\n");
				} else {
					out = clampToCeiling(in * totalGain);
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
	gWatchLoopGain = loopGain;

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
