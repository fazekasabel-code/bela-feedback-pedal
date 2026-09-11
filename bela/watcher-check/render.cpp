/*
 * watcher-check — Phase 1 instrumentation smoke test for bela-feedback-pedal.
 *
 * Resolves ground-rules-and-facts.md §3.1's open question 3: does the BelaPlatform
 * Watcher library + pybela (websocket streaming of Bela variables to Python) work on
 * this Gem Stereo / PocketBeagle 2 board?
 *
 * Streams the two audio input channels out over Watcher/pybela at audio rate.
 * Deliberately never calls audioWrite() -- output stays silent, so this cannot drive
 * the exciter no matter how the outputs are patched. Not a real-loop test.
 *
 * Watcher.h / Watcher.cpp here are vendored from the `watcher` submodule of
 * https://github.com/BelaPlatform/pybela (commit 60a09e0, 2023-11-01), per pybela's
 * own instructions: the Watcher library is not part of Bela core and has to be
 * copied into each project that uses it. See WATCHER-LICENSE for its license.
 *
 * pybela's own docs say this Watcher version "currently only works with the Bela
 * dev branch"; this board is on master (commit fb362a5, 2025-03-29). Needed two
 * fixes to get it to compile here -- see the two extra #includes below and the
 * <Bela.h>-before-<Watcher.h> order, both different from pybela's own tutorial:
 *   - <array> wasn't pulled in transitively, so std::array<Stream, N> failed to
 *     instantiate.
 *   - rt_fprintf (declared in Bela.h) wasn't visible yet when Watcher.h's inline
 *     methods that call it were parsed, because pybela's tutorials include
 *     Watcher.h *before* Bela.h. Including Bela.h first fixes it.
 */

#include <array>
#include <Bela.h>
#include <Watcher.h>
Watcher<float> gWatchIn0("audio_in_ch0");
Watcher<float> gWatchIn1("audio_in_ch1");

bool setup(BelaContext *context, void *userData)
{
	Bela_getDefaultWatcherManager()->getGui().setup(context->projectName);
	Bela_getDefaultWatcherManager()->setup(context->audioSampleRate);

	rt_printf("watcher-check\n");
	rt_printf("  audio  : %u in / %u out @ %.1f Hz, block %u\n",
	          context->audioInChannels, context->audioOutChannels,
	          context->audioSampleRate, context->audioFrames);
	rt_printf("  output : silent (not written) -- instrumentation-only check\n");
	rt_printf("  watching: audio_in_ch0, audio_in_ch1 over Watcher/pybela\n");

	return true;
}

void render(BelaContext *context, void *userData)
{
	const bool haveCh1 = context->audioInChannels > 1;

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		const uint64_t frames = context->audioFramesElapsed + n;
		Bela_getDefaultWatcherManager()->tick(frames);

		gWatchIn0 = audioRead(context, n, 0);
		if(haveCh1) gWatchIn1 = audioRead(context, n, 1);
	}

	// Output left untouched -- never call audioWrite() here.
}

void cleanup(BelaContext *context, void *userData)
{
}
