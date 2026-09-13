/*
 * latency-check — Phase 1 round-trip latency measurement for bela-feedback-pedal.
 *
 * ground-rules-and-facts.md 3.1/11's open question: total loop round-trip delay
 * including the mechanical path, which sets the spacing of the phase-condition
 * comb (docs/ground-rules-and-facts.md 7/11). Abel's suggestion, 2026-09-13:
 * play a short, quiet burst out through the exciter and listen for it on the
 * pickup -- this measures the WHOLE round trip (Bela DAC -> DTA120 -> exciter ->
 * body -> strings -> pickup -> Bela ADC) in one shot, electrical and
 * mechanical/acoustic legs together, which is what the phase condition actually
 * depends on -- not just Bela's own electrical latency.
 *
 * Emits kNumBursts short Hann-windowed tone bursts, kBurstSpacingS apart, on
 * the exciter's output channel (kGuitarInputChannel's OUTPUT counterpart --
 * see kExciterOutputChannel, rig-profile.json host.exciter_output_index).
 * Amplitude is deliberately quiet (kBurstAmp) -- Abel: "quiet for safety" --
 * this only needs to be found by cross-correlation offline, not heard loud.
 * Records both channels throughout; the host side (host/rig/analyse_latency.py)
 * finds each burst's arrival time on the pickup channel by normalised cross-
 * correlation (same method as host/rig/io_check.py's cmd_latency, adapted for
 * a real mechanical/acoustic path instead of a direct cable loopback).
 *
 * SAFETY: same shape as every other diagnostic here -- ceiling, watchdog,
 * mute-on-non-finite. kBurstAmp is a measurement parameter, not a safety
 * constant; kOutputCeiling still clamps everything regardless.
 */

#include <Bela.h>
#include <libraries/AudioFile/AudioFile.h>
#include <cmath>
#include <string>
#include <vector>

// ---------------------------------------------------------------- constants

static const float kOutputCeiling = 0.5f;
static const float kWatchdogTimeoutS = 30.0f;

// The guitar is mono, always on this input channel (rig-profile.json
// host.guitar_input_index, confirmed 2026-09-13).
static const unsigned int kGuitarInputChannel = 0;

// The exciter is fed from this output channel on this rig (rig-profile.json
// host.exciter_output_index -- as wired per Abel, not yet independently
// cross-checked; this measurement doubles as that cross-check, since a burst
// arriving back on the pickup at all proves the whole physical chain).
static const unsigned int kExciterOutputChannel = 1;

static const float kBurstAmp = 0.08f;     // quiet, per Abel -- ~-22 dBFS
static const float kBurstHz = 1000.0f;
static const float kBurstDurationS = 0.002f;  // 2 ms, Hann-windowed
static const float kBurstSpacingS = 1.0f;
static const int   kNumBursts = 5;
static const float kLeadInS = 0.3f;

static const std::string kInputsFilename = "inputs.wav";
static const std::string kOutputsFilename = "outputs.wav";
static const size_t kAudioFileBufferSize = 16384;

// ------------------------------------------------------------------- state

static bool     gMuted = false;
static uint64_t gFrameCount = 0;
static int      gBurstsEmitted = 0;

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

	gMuted = false;
	gFrameCount = 0;
	gBurstsEmitted = 0;

	rt_printf("latency-check\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  burst  : %.0f Hz, %.1f ms, amp %.3f, on out ch%u (exciter)\n",
	          kBurstHz, 1000.0f * kBurstDurationS, kBurstAmp, kExciterOutputChannel);
	rt_printf("  %d bursts, %.1f s apart, %.1f s lead-in\n",
	          kNumBursts, kBurstSpacingS, kLeadInS);
	rt_printf("  recording: %s (in, pickup on ch%u), %s (out)\n",
	          kInputsFilename.c_str(), kGuitarInputChannel, kOutputsFilename.c_str());

	return true;
}

// ------------------------------------------------------------------ render

void render(BelaContext *context, void *userData)
{
	const float elapsedS = (float)gFrameCount / context->audioSampleRate;

	if(!gMuted && elapsedS >= kWatchdogTimeoutS) {
		gMuted = true;
		rt_printf("watchdog timeout at %.1f s -- output muted\n", elapsedS);
	}

	const bool haveCh1Out = context->audioOutChannels > 1;
	const int burstDurationFrames = (int)(kBurstDurationS * context->audioSampleRate);

	for(unsigned int n = 0; n < context->audioFrames; n++) {

		const float in = audioRead(context, n, kGuitarInputChannel);
		gInputBuf[n * context->audioInChannels + 0] = in;
		if(context->audioInChannels > 1) gInputBuf[n * context->audioInChannels + 1] = 0.0f;

		float exciterOut = 0.0f;

		if(!gMuted) {
			if(!std::isfinite(in)) {
				gMuted = true;
				rt_printf("non-finite input sample -- output muted\n");
			} else if(gBurstsEmitted < kNumBursts) {
				const float nextBurstStartS = kLeadInS + gBurstsEmitted * kBurstSpacingS;
				const int64_t nextBurstStartFrame = (int64_t)(nextBurstStartS * context->audioSampleRate);
				const int64_t thisFrame = (int64_t)(gFrameCount + n);
				const int64_t intoburst = thisFrame - nextBurstStartFrame;

				if(intoburst >= 0 && intoburst < burstDurationFrames) {
					// Hann-windowed tone burst -- see header comment.
					const float phase = 2.0f * (float)M_PI * kBurstHz * (float)intoburst
					                     / context->audioSampleRate;
					const float window = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)intoburst
					                              / (float)(burstDurationFrames - 1)));
					exciterOut = clampToCeiling(kBurstAmp * sinf(phase) * window);
					if(intoburst == burstDurationFrames - 1) {
						gBurstsEmitted++;
						rt_printf("t=%.3fs  burst %d/%d emitted\n",
						          elapsedS, gBurstsEmitted, kNumBursts);
					}
				}
			}
		}

		for(unsigned int ch = 0; ch < context->audioOutChannels; ch++) {
			const float out = (ch == kExciterOutputChannel || !haveCh1Out) ? exciterOut : 0.0f;
			audioWrite(context, n, ch, out);
			gOutputBuf[n * context->audioOutChannels + ch] = out;
		}
	}

	gInputWriter.setSamples(gInputBuf);
	gOutputWriter.setSamples(gOutputBuf);

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
