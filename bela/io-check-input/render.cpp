/*
 * io-check-input — Phase 0 diagnostic for bela-feedback-pedal.
 *
 * Reads audio input only, no output. Prints a peak/RMS level meter per
 * channel once a second so Abel can confirm a signal is reaching the
 * Bela Gem audio inputs (e.g. pink noise fed in) before anything is wired
 * to drive an exciter.
 *
 * Deliberately does NOT write to audioWrite() at all — output stays fully
 * silent, so this cannot close any physical loop through a power amp or
 * exciter no matter how the board's outputs are patched. Not a real-loop
 * test; ground rule 2 does not apply.
 *
 * Not part of the gen1 DSP chain. Throwaway instrumentation — safe to
 * delete once Phase 1 brings in Watcher/pybela properly.
 */

#include <Bela.h>
#include <cmath>

static const unsigned int kReportEveryNBlocks = 0; // computed in setup() from sample rate
static unsigned int gBlocksPerReport = 0;
static unsigned int gBlockCount = 0;

static const unsigned int kMaxChannels = 8;
static float gPeak[kMaxChannels];
static double gSumSq[kMaxChannels];
static unsigned int gSampleCount = 0;

bool setup(BelaContext *context, void *userData)
{
	rt_printf("io-check-input\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  output : silent (not written) -- input-only check\n");

	for(unsigned int ch = 0; ch < kMaxChannels; ch++) {
		gPeak[ch] = 0.0f;
		gSumSq[ch] = 0.0;
	}

	if(context->audioFrames > 0) {
		gBlocksPerReport = (unsigned int)(context->audioSampleRate / context->audioFrames);
		if(gBlocksPerReport == 0) gBlocksPerReport = 1;
	} else {
		gBlocksPerReport = 1;
	}

	return true;
}

void render(BelaContext *context, void *userData)
{
	const unsigned int nCh = (context->audioInChannels < kMaxChannels)
	                          ? context->audioInChannels : kMaxChannels;

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		for(unsigned int ch = 0; ch < nCh; ch++) {
			const float in = audioRead(context, n, ch);
			const float a = std::fabs(in);
			if(a > gPeak[ch]) gPeak[ch] = a;
			gSumSq[ch] += (double)in * (double)in;
		}
		gSampleCount++;
	}

	// Output left untouched -- Bela zero-fills audioOut buffers by default,
	// but be explicit: never write a sample here.

	gBlockCount++;
	if(gBlockCount >= gBlocksPerReport) {
		gBlockCount = 0;
		rt_printf("level:");
		for(unsigned int ch = 0; ch < nCh; ch++) {
			const double rms = (gSampleCount > 0) ? std::sqrt(gSumSq[ch] / gSampleCount) : 0.0;
			const double peakDb = (gPeak[ch] > 1e-9f) ? 20.0 * log10((double)gPeak[ch]) : -999.0;
			const double rmsDb  = (rms > 1e-9) ? 20.0 * log10(rms) : -999.0;
			rt_printf("  ch%u peak=%.4f (%.1f dBFS) rms=%.4f (%.1f dBFS)",
			          ch, gPeak[ch], peakDb, rms, rmsDb);
			gPeak[ch] = 0.0f;
			gSumSq[ch] = 0.0;
		}
		rt_printf("\n");
		gSampleCount = 0;
	}
}

void cleanup(BelaContext *context, void *userData)
{
}
