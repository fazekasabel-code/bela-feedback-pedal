/*
 * gen1-passthrough — Phase 0 hello-world for the bela-feedback-pedal project.
 *
 * This is the skeleton every later version grows from, and the first thing that
 * ever drives the exciter. It does four things and nothing else:
 *
 *   1. passes audio in -> audio out
 *   2. enforces a hard output ceiling
 *   3. mutes permanently on any non-finite sample
 *   4. mutes permanently once the run exceeds its time box (watchdog)
 *
 * SAFETY: kOutputCeiling is a safety constant. No automatic tuning process may
 * raise it. It changes only by an explicit human commit. kWatchdogTimeoutS is
 * likewise not a tuning knob. See CLAUDE.md rules 1 and 3, ground-rules §4.5
 * and §10.1-10.2.
 *
 * There is no host heartbeat channel yet (that is pybela/Watcher, Phase 1), so
 * this watchdog is a plain hard cap on run length rather than "no heartbeat in
 * N seconds" — strictly the stronger of the two, which is the right direction
 * to round in the absence of the real mechanism.
 *
 * STATUS: NOT YET COMPILED OR RUN ON HARDWARE. Phase 0 exit criterion is that
 * this builds, runs, and passes signal.
 */

#include <Bela.h>
#include <cmath>

// ---------------------------------------------------------------- constants

// Linear output ceiling. 0.5 ~= -6 dBFS. SAFETY CONSTANT — see header comment.
static const float kOutputCeiling = 0.5f;

// Hard time box for any run of this program. SAFETY CONSTANT — see header
// comment. Matches rig-profile.json safety.watchdog_timeout_s.
static const float kWatchdogTimeoutS = 120.0f;

// Analog input carrying the expression pedal (Phase 6). Read but unused for now.
static const unsigned int kExpressionPedalChannel = 0;

// ------------------------------------------------------------------- state

static bool   gMuted = false;         // latches true on error; never clears at runtime
static float  gExpressionRaw = 0.0f;  // 0..~0.806 for a 3.3 V-fed pedal, see docs 4.3
static uint64_t gFrameCount = 0;      // audio frames rendered since setup()

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
	gFrameCount = 0;

	rt_printf("gen1-passthrough\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  analog : %u in @ %.1f Hz, %u frames\n",
	          context->analogInChannels, context->analogSampleRate,
	          context->analogFrames);
	rt_printf("  ceiling: %.3f linear\n", kOutputCeiling);
	rt_printf("  watchdog: mutes at %.0f s\n", kWatchdogTimeoutS);

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

	// Watchdog: once the run has been going longer than its time box, latch
	// mute for good, same as the non-finite-sample path below. Checked once
	// per block, not per sample -- fine, the ceiling is 120 s wide.
	if(!gMuted) {
		const float elapsedS = (float)gFrameCount / context->audioSampleRate;
		if(elapsedS >= kWatchdogTimeoutS) {
			gMuted = true;
			rt_printf("watchdog timeout at %.1f s — output muted\n", elapsedS);
		}
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

	gFrameCount += context->audioFrames;
}

// ----------------------------------------------------------------- cleanup

void cleanup(BelaContext *context, void *userData)
{
}
