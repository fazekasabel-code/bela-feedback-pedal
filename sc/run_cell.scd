/*
Headless runner for gen1_cell. Driven entirely by environment variables so it can be
scripted from the host harness — see host/rig/sc_run.py, which is the intended way in.

    GEN1_CONFIRM=1 GEN1_SECONDS=30 GEN1_GAIN_END=0.05 \
      /Applications/SuperCollider.app/Contents/MacOS/sclang -D sc/run_cell.scd

Refuses to run without GEN1_CONFIRM=1, because this drives a live exciter.

It boots the server, loads the SynthDefs, ramps the master gain up from silence,
records the take, collects the cell's telemetry at 20 Hz, prints one JSON object on
stdout between the markers ---GEN1-JSON--- and ---END---, and exits. The JSON is what
the host reads; everything else on stdout is SuperCollider noise.
*/

(
var envStr, envNum, dev, secs, ramp, gain0, gain1, recPath, dir;
var actuator, bypass, targetDb, rq, attack, release, maxCut, timebox, telePath;
var tele, done;

envStr = { |key, dflt|
    var v = key.getenv;
    if(v.isNil or: { v.isEmpty }) { dflt } { v };
};
envNum = { |key, dflt|
    var v = key.getenv;
    if(v.isNil or: { v.isEmpty }) { dflt } { v.asFloat };
};

if(envStr.("GEN1_CONFIRM", "0") != "1") {
    "gen1: refusing to run without GEN1_CONFIRM=1 (this drives a live exciter).".postln;
    0.exit;
};

dir      = thisProcess.nowExecutingPath.dirname;
dev      = envStr.("GEN1_DEVICE", "Scarlett 8i6 USB");
secs     = envNum.("GEN1_SECONDS", 30);
ramp     = envNum.("GEN1_RAMP", 8);
gain0    = envNum.("GEN1_GAIN", 0.0);
gain1    = envNum.("GEN1_GAIN_END", 0.02);
recPath  = envStr.("GEN1_REC", "/tmp/gen1_take.wav");
telePath = envStr.("GEN1_TELE", recPath ++ ".telemetry.csv");
actuator = envNum.("GEN1_ACTUATOR", 0);
bypass   = envNum.("GEN1_BYPASS", 0);
targetDb = envNum.("GEN1_TARGET_DB", -24);
rq       = envNum.("GEN1_RQ", 0.1);
attack   = envNum.("GEN1_ATTACK", 0.003);
release  = envNum.("GEN1_RELEASE", 0.4);
maxCut   = envNum.("GEN1_MAXCUT", 30);
timebox  = secs + 10;   // the in-server watchdog outlives the script, but not by much

// The master gain is the only thing that decides how hard this pushes the exciter.
// A typo in an env var should not be able to make it loud.
if(gain1 > 0.35) {
    ("gen1: refusing GEN1_GAIN_END=" ++ gain1 ++ ", above the 0.35 script limit.").postln;
    0.exit;
};

s = Server.default;
s.options.device = dev;
s.options.numInputBusChannels = 2;
s.options.numOutputBusChannels = 4;
s.options.sampleRate = 48000;
s.options.hardwareBufferSize = 128;
s.options.memSize = 65536;

tele = List.new;
done = false;

s.waitForBoot({
    var cell, rec, buf, t0, summary, freqs, cuts, inDbs, pick;

    (dir +/+ "gen1_cell.scd").load;
    s.sync;

    // A SynthDef that failed to compile does not raise here — the Synth() below would
    // allocate a node id, the server would refuse it, every /n_set would fail, and the
    // run would still finish and report success having made no sound and no data.
    // That is the worst possible outcome, so check explicitly.
    if(SynthDescLib.global[\gen1_cell].isNil or: { SynthDescLib.global[\gen1_rec].isNil }) {
        "---GEN1-JSON---".postln;
        "( ok: false, error: \"SynthDefs did not compile - see the errors above\" )".postln;
        "---END---".postln;
        s.quit; 0.5.wait; 0.exit;
    };

    buf = Buffer.alloc(s, 65536, 2);
    buf.write(recPath, "wav", "int16", 0, 0, true);
    s.sync;

    OSCdef(\gen1tele, { |msg|
        // [ '/gen1', nodeID, replyID, freq, hasFreq, partialDb, cutDb, inDb ]
        tele.add(msg[3..7]);
    }, '/gen1');

    cell = Synth(\gen1_cell, [
        \inBus, 0, \outBus, 2, \masterGain, gain0,
        \actuator, actuator, \bypass, bypass,
        \targetDb, targetDb, \rq, rq, \attack, attack, \release, release,
        \maxCutDb, maxCut, \timebox, timebox,
    ], s, \addToHead);

    rec = Synth(\gen1_rec, [
        \inBus, 0, \outBus, 2, \bufnum, buf.bufnum, \timebox, timebox,
    ], s, \addToTail);

    t0 = Main.elapsedTime;

    Routine({
        var f;
        // Ramp up from silence. Never start where the last run finished.
        var steps = (ramp * 20).max(1).asInteger;
        steps.do { |i|
            cell.set(\masterGain, gain0 + ((gain1 - gain0) * (i + 1) / steps));
            0.05.wait;
        };
        (secs - ramp).max(0).wait;

        // Ramp back down rather than cutting, so the take ends without a click.
        20.do { |i| cell.set(\masterGain, gain1 * (1 - ((i + 1) / 20))); 0.02.wait };
        0.2.wait;

        rec.free; cell.free;
        s.sync;
        buf.close; buf.free;
        s.sync;

        // ---- summary -------------------------------------------------------------
        pick  = { |ix| tele.collect({ |t| t[ix].asFloat }) };
        freqs = pick.(0); cuts = pick.(3); inDbs = pick.(4);

        // The cell reports at 20 Hz. No telemetry means the synth never ran, whatever
        // else went right — never call that a successful take.
        summary = (
            ok: tele.size > 0,
            error: if(tele.size > 0) { "" } { "no telemetry: the cell synth never ran" },
            seconds: (Main.elapsedTime - t0).round(0.01),
            telemetry_frames: tele.size,
            params: (
                actuator: actuator, bypass: bypass, target_db: targetDb,
                rq: rq, attack: attack, release: release, max_cut_db: maxCut,
                gain_start: gain0, gain_end: gain1, ramp_s: ramp, seconds: secs,
                device: dev
            ),
            audio_path: recPath
        );

        if(tele.size > 0) {
            summary[\freq_median]  = freqs.median.round(0.01);
            summary[\freq_min]     = freqs.minItem.round(0.01);
            summary[\freq_max]     = freqs.maxItem.round(0.01);
            summary[\cut_db_mean]  = (cuts.sum / cuts.size).round(0.01);
            summary[\cut_db_max]   = cuts.minItem.round(0.01);   // most negative = deepest
            summary[\in_db_max]    = inDbs.maxItem.round(0.01);
            summary[\cell_active_fraction] =
                (cuts.count({ |c| c < -0.5 }) / cuts.size).round(0.001);
        };

        // Dump the whole trajectory, not just its summary. Working out WHY a take did
        // what it did needs the shape over time — when the cell armed, whether the
        // frequency wandered, when the cut backed off — and a median cannot show that.
        // Frames arrive at a fixed 20 Hz, so index/20 is the time since the synth started.
        if(tele.size > 0) {
            f = File(telePath, "w");
            if(f.isOpen) {
                f.write("t_s,freq_hz,has_freq,partial_db,cut_db,in_db\n");
                tele.do { |row, i|
                    f.write("%,%,%,%,%,%\n".format(
                        (i / 20).round(0.05), row[0].round(0.01), row[1].round(0.01),
                        row[2].round(0.01), row[3].round(0.01), row[4].round(0.01)));
                };
                f.close;
                summary[\telemetry_path] = telePath;
            } {
                ("gen1: could not open " ++ telePath ++ " for the telemetry dump").postln;
            };
        };

        "---GEN1-JSON---".postln;
        summary.asCompileString.postln;
        "---END---".postln;

        done = true;
        0.3.wait;
        s.quit;
        0.5.wait;
        0.exit;
    }).play(AppClock);
});

// Hard backstop: if anything above wedges, do not sit holding the interface forever.
SystemClock.sched(secs + 30, {
    if(done.not) {
        "---GEN1-JSON---".postln;
        "( ok: false, error: \"timed out\" )".postln;
        "---END---".postln;
        s.quit;
        0.exit;
    };
    nil
});
)
