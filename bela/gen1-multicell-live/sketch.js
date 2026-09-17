// gen1-multicell-live's own browser view, 2026-09-14 -- replaces the generic
// Watcher control table (vendored from github.com/BelaPlatform/watcher's own
// sketch.js) with a purpose-built one: a 12-cell grid, a summary row, and
// (added same day) live tuning sliders for the parameters worth exploring
// while playing, instead of a 53-row table of every watcher variable with
// Watch/Control/Log checkboxes most of which are irrelevant here.
//
// The wire protocol below (sendCommand/requestList/controlCallback, and the
// {cmd:"set", watchers:[...], values:[...]} shape the sliders send) is the
// same mechanism the generic sketch.js used -- that part is genuinely how
// Bela's Gui/Watcher talks to the board, not something to reinvent. What's
// different is reading each variable's plain `value` field straight off the
// periodic "list" response instead of building the full per-variable
// watch/control/log/mask/monitor UI, and drawing a small fixed dashboard
// instead of an auto-generated table.
//
// The sliders below map 1:1 to Watcher<float> variables in render.cpp
// that are actually settable -- see that file's "localControl(false)" comment
// for why a Watcher variable needs that call before a browser "set" does
// anything at all (found the hard way: earlier host-controlled variables in
// this repo, e.g. detector-passthrough's `gain`, never called it and so were
// never actually controllable from here despite being named like they were).
// render.cpp clamps every one of these in code regardless of the slider's own
// range -- this UI's min/max are a starting point for exploration, not the
// safety boundary.

let values = {};       // watcher name -> latest value (from the periodic "list")
let sampleRate = 0;
let connected = false;

const kNumCells = 12;
const kRefreshMs = 300;   // faster than the generic sketch's stock 1234ms

// ---- wire protocol ----------------------------------------------------

function sendCommand(cmd) {
	Bela.control.send({ watcher: Array.isArray(cmd) ? cmd : [cmd] });
}

function requestList() {
	sendCommand({ cmd: "list" });
}

function setWatcher(name, value) {
	sendCommand({ cmd: "set", watchers: [name], values: [value] });
}

function controlCallback(data) {
	if (!data.watcher || !data.watcher.watchers) return;
	const firstConnect = !connected;
	connected = true;
	sampleRate = data.watcher.sampleRate;
	for (const w of data.watcher.watchers) {
		values[w.name] = w.value;
	}
	if (firstConnect) syncControlsFromBoard();
	setTimeout(requestList, kRefreshMs);
}

// ---- controls -----------------------------------------------------------
// name -> {el, kind: 'slider'|'checkbox', label, min, max, step, fmt}
let controls = {};

function addSlider(name, label, min, max, step, x, y, fmt) {
	const el = createSlider(min, max, min, step);
	el.position(x, y + 14);
	el.style('width', '150px');
	el.input(() => setWatcher(name, el.value()));
	controls[name] = { el, kind: 'slider', label, x, y, fmt };
}

function addCheckbox(name, label, x, y) {
	const el = createCheckbox('', false);
	el.position(x, y + 14);
	el.changed(() => setWatcher(name, el.checked() ? 1 : 0));
	controls[name] = { el, kind: 'checkbox', label, x, y };
}

// Sliders/checkboxes start at an arbitrary default until the board's real
// current value arrives -- move them into place once, on first connection,
// rather than fighting the user's own dragging on every list refresh after.
function syncControlsFromBoard() {
	for (const name in controls) {
		const c = controls[name];
		const v = values[name];
		if (typeof v !== 'number') continue;
		if (c.kind === 'slider') c.el.value(v);
		else c.el.checked(v !== 0);
	}
}

function setup() {
	createCanvas(windowWidth, windowHeight);
	textFont('Courier New');
	Bela.control.registerCallback("controlCallback", controlCallback, {});
	requestList();

	// Four columns, three rows. Row 1 is the cell law -- where a partial is
	// driven TO and how hard a cell may pull it down -- plus the master upward
	// unit, which is the one control here that changes how much energy the loop
	// carries. Row 2 is what the allocator will even look at. Row 3 is the
	// release-side anti-chatter pair and the output stage.
	const row1 = 150, row2 = 200, row3 = 250, row4 = 300, colW = 210;
	addSlider('target_db',       'target (dB)',       -48, -6,   0.5, 16,            row1);
	addSlider('max_cut_db',      'max cut (dB)',        3, 40,   0.5, 16 + colW,     row1);
	addSlider('master_boost_db', 'MASTER boost (dB)', -12, 30,   0.5, 16 + colW * 2, row1);
	// The reduction profile (2026-09-17). slope is the "ratio" knob: +1 = cell
	// off, 0 = brick wall pinned at the target (what this did before), negative =
	// over-compression, pushed BELOW the target and further below the louder it
	// gets. Measured in loop_sim.py: slope recruits no extra partials, but it
	// collapses the spread between the ones you have -- 13 dB down to 6.5 dB --
	// which is what makes several pitches read as a chord instead of one pitch
	// with whispers under it. MASTER recruits, SLOPE evens out.
	addSlider('reduction_slope', 'SLOPE (dB/dB)',      -3, 1,    0.05, 16 + colW * 3, row1);
	addSlider('release_ms',      'release (ms)',       50, 3000, 10,  16,            row2);
	addSlider('cell_q',          'Q',                   2, 30,   0.5, 16 + colW,     row2);
	addSlider('prominence_db',   'prominence (dB)',     3, 30,   0.5, 16 + colW * 2, row2);
	addSlider('knee_db',         'knee (dB)',           0, 24,   0.5, 16 + colW * 3, row2);
	// Row 3: the release-side anti-chatter pair (see render.cpp's
	// kMinBoundHoldFrames comment -- cells were letting go of their partial at the
	// earliest frame the code allows, ~280 ms, over and over), then the output.
	addSlider('min_hold_ms',      'min hold (ms)',      50, 3000, 10,  16,            row3);
	addSlider('release_margin_db','release margin (dB)', 3, 40,   0.5, 16 + colW,     row3);
	addSlider('loop_gain',        'loop gain (x)',       0, 1.5,  0.01, 16 + colW * 2, row3);
	addCheckbox('bypass',         'bypass',                             16 + colW * 3, row3);
	// Row 4: the envelope's attack, live as of 2026-09-17 (see render.cpp's
	// kAttackMs comment for why it stopped being a fixed safety constant). This is
	// the plucked-note knob. Default is 300 ms as of 2026-09-17, Abel's judgement
	// after a live session. At the old 3 ms the envelope tracked the pluck
	// TRANSIENT, which sits 20-30 dB above the level the note sustains at, so the
	// cut slammed on inside the pluck and flattened the attack of every note. Slow
	// it down and the transient passes while sustained growth is still regulated --
	// feedback grows at ~1-2.5 dB/s, so even 500 ms overshoots by only ~1.25 dB.
	// At 300 ms against a 400 ms release the envelope is near-symmetric; if the cut
	// starts pumping with the playing rather than the feedback, raise release_ms.
	addSlider('attack_ms',        'ATTACK (ms)',         1, 500,  1,   16,            row4);
}

function windowResized() {
	resizeCanvas(windowWidth, windowHeight);
}

function num(name, digits) {
	const v = values[name];
	return (typeof v === 'number' && isFinite(v)) ? v.toFixed(digits) : '--';
}

function draw() {
	background(18);

	if (!connected) {
		fill(180);
		noStroke();
		textSize(16);
		text('connecting...', 16, 30);
		return;
	}

	noStroke();
	fill(235);
	textSize(20);
	text('gen1-multicell-live', 16, 30);
	fill(140);
	textSize(12);
	text(sampleRate ? sampleRate + ' Hz' : '', 16, 48);

	// ---- summary row ----------------------------------------------------
	const boundCount = values['cells_bound_count'];
	const muted = values['muted'];
	const summaryY = 78;
	textSize(14);
	fill(235);
	text('bound ' + (typeof boundCount === 'number' ? boundCount.toFixed(0) : '--') + '/' + kNumCells, 16, summaryY);
	text('in '  + num('in_peak', 3),  150, summaryY);
	text('out ' + num('out_peak', 3), 280, summaryY);
	text('cpu ' + num('audio_thread_cpu_percent', 1) + '%', 410, summaryY);
	fill(muted ? color(230, 90, 90) : color(90, 210, 110));
	text(muted ? 'MUTED' : 'live', 540, summaryY);
	fill(150);
	text('bind/release/steal ' + num('bind_events_total', 0) + '/'
	     + num('release_events_total', 0) + '/' + num('steal_events_total', 0), 16, summaryY + 20);

	// ---- tuning controls --------------------------------------------------
	textSize(12);
	for (const name in controls) {
		const c = controls[name];
		// The master upward unit is picked out: it is the only control here that
		// changes how much energy the loop carries, and so the direct lever on how
		// many partials sustain. Everything else decides what the cells see and
		// how steadily they hold it.
		// The three controls that do the three different jobs: master recruits
		// modes, slope decides whether they sit at comparable level, and attack
		// decides whether a pluck's transient is regulated or passes through.
		const hot = (name === 'master_boost_db' || name === 'reduction_slope'
		             || name === 'attack_ms');
		fill(hot ? color(235, 200, 110) : color(190));
		const suffix = (name === 'loop_gain' || name === 'reduction_slope')
			? num(name, 2) : num(name, 1);
		text(c.label + (c.kind === 'slider' ? '  ' + suffix : ''), c.x, c.y + 10);
	}

	// ---- per-cell grid ------------------------------------------------------
	const cols = 4;
	const gridTop = 360, cellW = 190, cellH = 92, pad = 10;
	for (let c = 0; c < kNumCells; c++) {
		const col = c % cols;
		const row = Math.floor(c / cols);
		const x = 16 + col * (cellW + pad);
		const y = gridTop + row * (cellH + pad);
		const bound = values['cell' + c + '_bound'];

		noStroke();
		fill(bound ? color(35, 85, 48) : color(32, 32, 36));
		rect(x, y, cellW, cellH, 6);

		textSize(13);
		fill(bound ? color(130, 235, 150) : color(110));
		text('cell ' + c, x + 10, y + 20);

		textSize(12);
		if (bound) {
			fill(225);
			text(num('cell' + c + '_freq_hz', 0) + ' Hz', x + 10, y + 42);
			text('cut  ' + num('cell' + c + '_cut_db', 1) + ' dB', x + 10, y + 60);
			text('lvl  ' + num('cell' + c + '_partial_db', 1) + ' dBFS', x + 10, y + 78);
		} else {
			fill(85);
			text('free', x + 10, y + 42);
		}
	}
}
