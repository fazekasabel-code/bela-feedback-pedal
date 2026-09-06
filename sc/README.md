# SuperCollider on the Mac rig — verified server config

Verified 2026-09-06 on this machine, SuperCollider 3.14.1. No audio was ever sent to an
output bus during this check — every test SynthDef had no `Out.ar` at all.

## Binaries

- `sclang`:  `/Applications/SuperCollider.app/Contents/MacOS/sclang`
- `scsynth`: `/Applications/SuperCollider.app/Contents/Resources/scsynth`

## Device name

`ServerOptions.devices` lists the Scarlett once, under one name, in both `.inDevices` and
`.outDevices`: `"Scarlett 8i6 USB"`. So `s.options.device` takes that single string — no
`"in:out"` pair needed here. That pair syntax is for when in/out devices differ; not this rig.

## ServerOptions block (boots clean)

```supercollider
s = Server.default;
s.options.device = "Scarlett 8i6 USB";
s.options.numInputBusChannels = 2;
s.options.numOutputBusChannels = 4;
s.options.sampleRate = 48000;
s.options.hardwareBufferSize = 128;
// memSize: default (8192 KB) was enough with no synths running. Not raised.
s.waitForBoot({ /* server is up here */ });
```

Boot log confirms `SC_AudioDriver: sample rate = 48000.000000, driver's block size = 128`
and `SuperCollider 3 server ready.`, and `s.serverRunning` reads `true` inside the
`waitForBoot` callback — that's how "running" was confirmed, not just "process launched."
Hardware reports 10 input / 6 output channels available; we only opened 2 in / 4 out.

## Boot / quit commands

Boot headless: `sclang -D yourfile.scd`, with the block above plus a `waitForBoot`
callback. sclang does not exit on its own — call `0.exit` explicitly when done.

Quit cleanly: `s.quit` from sclang. Kill a stray process from the shell: `pkill -f scsynth`.
Verified no stray `scsynth` or `sclang` process was left after `s.quit` + `0.exit`.

## Can SuperCollider and Python/sounddevice share the device?

**Yes, verified.** Booted scsynth holding the Scarlett, then in a separate shell, with
scsynth still up, ran `cd host && python3 -m rig.io_check meter --channel 0 --seconds 3`.
It completed normally (exit 0, printed RMS/peak meter lines) while scsynth held the
device. No error, no xrun logged on the SC side either. CoreAudio here does not put the
interface into an exclusive-mode lock the way some ASIO drivers do — both processes had
streams open on it at once.

**Not verified:** output streams from both processes at once. This test only ran an
input-only Python stream against a silent scsynth. Don't assume that generalizes to two
processes both writing to outputs 3/4 — untested, and not something to find out by trying
it live on this rig.
