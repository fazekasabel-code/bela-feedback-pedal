// NOTE (bela-feedback-pedal, 2026-09-11): two lines added ahead of the original
// `#include <Watcher.h>` to compile against this board's Bela (master, not the
// `dev` branch pybela's docs call for) -- see render.cpp's header comment for why.
#include <array>
#include <Bela.h>
#include <Watcher.h>

WatcherManager* Bela_getDefaultWatcherManager()
{
	static Gui gui;
	static WatcherManager defaultWatcherManager(gui);
	return &defaultWatcherManager;
}
