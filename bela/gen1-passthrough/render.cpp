/*
 * gen1-passthrough — Phase 0 hello-world for the bela-feedback-pedal project.
 *
 * This is the skeleton every later version grows from, and the first thing that
 * ever drives the exciter. It does three things and nothing else:
 *
 *   1. passes audio in -> audio out
 *   2. enforces a hard output ceiling
 *   3. mutes permanently on any non-finite sample
 *
 * SAFETY: kOutputCeiling is a safety constant. No automatic tuning process may
 * raise it. It changes only by an explicit human commit. See CLAUDE.md.
 *
 * STATUS: NOT YET COMPILED OR RUN ON HARDWARE. Phase 0 exit criterion is that
 * this builds, runs, and passes signal.
 */

#include <Bela.h>
#include <cmath>

// ---------------------------------------------------------------- constants

// Linear output ceiling. 0.5 ~= -6 dBFS. SAFETY CONSTANT — see header comment.
static const float kOutputCeiling = 0.5f;

// Analog input carrying the expression pedal (Phase 6). Read but unused for now.
static const unsigned int kExpressionPedalChannel = 0;

// ------------------------------------------------------------------- state

static bool  gMuted = false;         // latches true on error; never clears at runtime
static float gExpressionRaw = 0.0f;  // 0..~0.806 for a 3.3 V-fed pedal, see docs 4.3

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
	gExpressionRaw = 0.0f;

	rt_printf("gen1-passthrough\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  analog : %u in @ %.1f Hz, %u frames\n",
	          context->analogInChannels, context->analogSampleRate,
	          context->analogFrames);
	rt_printf("  ceiling: %.3f linear\n", kOutputCeiling);

	return true;
}

// ------------------------------------------------------------------ render

void render(BelaContext *context, void *userData)
{
	// Expression pedal, read once per block and smoothed. Unused until Phase 6;
	// present now so Phase 1 can log its calibrated endpoints.
	if(context->analogFrames > 0 && context->analogInChannels > kExpressionPedalChannel) {
		const float raw = analogRead(context, 0, kExpressionPedalChannel);
		gExpressionRaw += 0.01f * (raw - gExpressionRaw);   // ~one-pole smoother
	}

	for(unsigned int n = 0; n < context->audioFrames; n++) {

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
					out = clampToCeiling(in);
				}
			}

			audioWrite(context, n, ch, out);
		}
	}
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
