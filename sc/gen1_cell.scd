/*
gen1 — one adaptive gain-regulation cell, SuperCollider.

    guitar -> Scarlett in 1 -> [ cell ] -> [ master ] -> [ ceiling ] -> out 3/4 -> amp
                                                                                   |
              pickup <- strings <- body <- exciter <----------------------------- /

Phase M3 of docs/mac-rig-plan.md. One cell, bound to whichever partial is currently
running away. The test this exists to answer is specific and audible:

    hold the dominant partial at its target, and a SECOND partial should appear on
    its own.

That is the inhomogeneous-saturation argument of ground-rules section 5, working or
not working. If no second partial arrives, the cell is either too shallow (the winner
still wins) or too deep (loop gain fell below unity everywhere and the feedback died).

WHAT THE ACTUATOR IS. Not a fixed-depth notch. The cut is driven by an envelope
follower comparing that partial's level to a target, so it applies exactly as much
reduction as it takes to hold the target and no more — "suppression" in this project
always means regulation to a level, never removal. A fixed notch is either too shallow
to stop the winner or deep enough to kill the note; there is no good depth to pick.

Two actuator shapes, so they can be A/B'd on the rig:
    \actuator = 0   peaking EQ with negative gain (ground-rules 6.3, the default)
    \actuator = 1   band-reject, crossfaded dry->notch by the same envelope

SAFETY — this patch drives a live exciter.
  * \ceiling is a hard constant here and in host/rig/loop.py. Ground rule 1: no
    automatic tuning process may raise it. Human commit only.
  * The synth carries its own watchdog: a Line.kr with doneAction 2 frees it after
    \timebox seconds. This is deliberately in the SERVER, not in sclang — if the
    language process dies or the network drops, the synth still stops itself.
  * Non-finite output is replaced with silence, not "recovered".
  * \masterGain defaults to effectively silence. Come up from there, never down to it.
*/

(
SynthDef(\gen1_cell, { |
    inBus = 0, outBus = 2,
    masterGain = 0.001,     // linear. Ramped; start inaudible and come up.
    bypass = 0,             // 1 = cell out of circuit. The raw winner-takes-all loop.
    actuator = 0,           // 0 = peaking EQ (default), 1 = band-reject crossfade

    targetDb = -24,         // the level this partial is allowed to reach
    rq = 0.1,               // reciprocal Q. 0.1 = Q 10. Narrow, but not surgical.
    attack = 0.003,         // must be faster than the loop's growth rate
    release = 0.4,          // how long a partial stays "used up". Musical parameter.
    maxCutDb = 30,          // the cell may never pull harder than this
    freqLag = 0.02,         // frequency slew. Not zero, or coefficient jumps click.

    ceiling = 0.7,          // HARD CONSTANT. Ground rule 1.
    timebox = 120           // watchdog, seconds. The synth frees itself.
    |
    var in, rawFreq, hasFreq, freq;
    var partial, partialDb, excessDb, cutDb, cutLin;
    var peaked, notched, wet, sig, bad;

    in = SoundIn.ar(inBus);

    // ---- detection -------------------------------------------------------------
    // Feedback locked on one mode is very nearly a pure sinusoid, which is the case
    // autocorrelation pitch tracking handles best. Gate on hasFreq so that when the
    // tracker is unsure the cell HOLDS its last frequency instead of chasing noise
    // into some unrelated part of the spectrum and notching a partial that was fine.
    # rawFreq, hasFreq = Pitch.kr(in,
        initFreq: 220, minFreq: 55, maxFreq: 2000,
        execFreq: 200, maxBinsPerOctave: 16, median: 5,
        ampThreshold: 0.004, peakThreshold: 0.5, downSample: 1);
    freq = Lag.kr(Gate.kr(rawFreq, hasFreq), freqLag).clip(55, 2000);

    // ---- how loud is that partial, right now -----------------------------------
    partial = BPF.ar(in, freq, rq);
    partialDb = Amplitude.kr(partial, attack, release).ampdb;

    // ---- the regulator ---------------------------------------------------------
    // Reduce by exactly the excess over target. At or below target the cell does
    // nothing at all, which is what makes this a regulator and not a notch.
    excessDb = (partialDb - targetDb).max(0).min(maxCutDb);
    cutDb = LagUD.kr(excessDb, attack, release).neg;

    // ---- actuator --------------------------------------------------------------
    peaked  = BPeakEQ.ar(in, freq, rq, cutDb);

    // Band-reject shape: same envelope, expressed as a dry/notch crossfade so the
    // depth is still driven by the level error rather than being fixed.
    cutLin  = cutDb.neg / maxCutDb;                  // 0 = no cut, 1 = full notch
    notched = XFade2.ar(in, BRF.ar(in, freq, rq), cutLin.linlin(0, 1, -1, 1));

    wet = Select.ar(actuator, [peaked, notched]);
    sig = Select.ar(bypass,   [wet, in]);

    // ---- master, ceiling, guards ------------------------------------------------
    sig = sig * Lag.kr(masterGain, 0.05);
    sig = Limiter.ar(sig, ceiling, 0.01);
    sig = LeakDC.ar(sig);
    sig = sig.clip2(ceiling);   // belt and braces behind the limiter

    // A NaN cannot be multiplied away — NaN * 0 is still NaN — so swap the signal
    // out entirely. CheckBadValues returns 1 nan, 2 inf, 3 denormal; only the first
    // two are faults worth muting for.
    bad = CheckBadValues.ar(sig, 0, 0);
    sig = Select.ar((bad > 0) * (bad < 3), [sig, DC.ar(0)]);

    // The watchdog. In the server on purpose: it survives sclang dying.
    Line.kr(0, 1, timebox, doneAction: 2);

    Out.ar(outBus, [sig, sig]);

    // Telemetry, so the host can see what the cell thinks without recompiling.
    SendReply.kr(Impulse.kr(20), '/gen1',
        [freq, hasFreq, partialDb, cutDb, Amplitude.kr(in).ampdb]);
}).add;

/*
Recorder. Captures the rig input and the DSP output as a 2-channel file, so a take
can be scored afterwards with `python3 -m rig.analyse_take <path>`. Channel 0 is the
pickup, channel 1 is what we sent to the exciter — the same layout host/rig/loop.py
writes, so one analysis tool reads both.
*/

SynthDef(\gen1_rec, { |inBus = 0, outBus = 2, bufnum = 0, timebox = 120|
    // Must run at the TAIL of the node order, after \gen1_cell has written, or In.ar
    // reads last block's output and the recorded output lags the input by one block.
    var pickup = SoundIn.ar(inBus);
    var sent = In.ar(outBus, 1);
    Line.kr(0, 1, timebox, doneAction: 2);
    DiskOut.ar(bufnum, [pickup, sent]);
}).add;
)
