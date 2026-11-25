/* Exploding Potatoes — Node test harness. Runs the raw C/Wasm module. */
const fs = require('fs');
const path = require('path');

let failures = 0;
function ok(cond, label) {
  console.log((cond ? 'PASS' : 'FAIL') + '  ' + label);
  if (!cond) failures++;
}

// The framebuffer is heap-allocated (not at byte 0); _game_buf() returns its base pointer.
// Not cached: init_game() frees + re-mallocs it, so the base can move.
function pixelAt(M, x, y) {
  const W = M._game_w();
  const o = (M._game_buf() | 0) + (y * W + x) * 4;
  return [M.HEAPU8[o], M.HEAPU8[o + 1], M.HEAPU8[o + 2]];
}
function hasColorNear(M, cellX, cellY, rgb) {
  const C = 32;
  const cx = cellX * C + C / 2, cy = cellY * C + C / 2;
  const h = 22;
  for (let y = cy - h; y <= cy + h; y++) {
    for (let x = cx - h; x <= cx + h; x++) {
      if (x < 0 || y < 0 || x >= M._game_w() || y >= M._game_h()) continue;
      const p = pixelAt(M, x, y);
      if (Math.abs(p[0] - rgb[0]) < 6 && Math.abs(p[1] - rgb[1]) < 6 && Math.abs(p[2] - rgb[2]) < 6)
        return true;
    }
  }
  return false;
}

async function main() {
  console.log('--- init ---');
  const factory = require(path.join(__dirname, 'potato.js'));
  const P = (typeof factory === 'function') ? await factory() : factory;

  P._init_game();
  ok(P._game_w() === 672, 'game width is 672');
  ok(P._game_h() === 672, 'game height is 672');
  ok(P._get_dead() === 0, 'not dead after init');
  ok(P._get_hearts() === 3, 'starts with 3 hearts');
  ok(P._get_score() === 0, 'starts at 0 score');

  // prime the game clock
  P._frame(1000);
  ok(true, 'frame primes without crashing');

  console.log('--- render: farmer at center ---');
  const CAP = [0x5c, 0xc3, 0x76];     // farmer's green cap
  const cx = 21 >> 1, cy = 21 >> 1;
  ok(hasColorNear(P, cx, cy, CAP), 'farmer pixels visible at center cell');

  // snapshot of the farmer's cell, to compare facings and walk frames
  const look = (x) => {
    let out = '';
    for (let dy = -34; dy < 22; dy++)
      for (let dx = -24; dx < 24; dx++) out += pixelAt(P, x * 32 + 16 + dx, cy * 32 + 16 + dy);
    return out;
  };
  // everything here stays under 700 ms of game time, before the first spawn
  const seen = new Set();
  for (const d of [0, 1, 2, 3]) {
    P._init_game(); P._frame(1000);
    P._step(d); P._frame(1200);                     // out ...
    P._step((d + 2) % 4); P._frame(1500);           // ... and back, facing the other way
    seen.add(look(cx));
  }
  ok(seen.size === 4, 'farmer looks different facing each direction (' + seen.size + ')');
  // where the farmer's green cap is drawn, as a mean x in pixels
  const capX = () => {
    let sum = 0, n = 0;
    for (let y = 0; y < 672; y++)
      for (let x = 0; x < 672; x++)
        if (String(pixelAt(P, x, y)) === String(CAP)) { sum += x; n++; }
    return n ? sum / n : NaN;
  };
  console.log('--- smooth movement ---');
  P._init_game(); P._frame(1000);
  const x0 = capX();
  P._step(1);
  const xs = [1025, 1050, 1075, 1120].map((now) => { P._frame(now); return capX(); });
  ok(xs[0] > x0 && xs.every((x, i) => i === 0 || x > xs[i - 1]), 'a step glides across frames (' + [x0, ...xs].map(Math.round).join(' ') + ')');
  P._frame(1300);
  ok(Math.abs(capX() - xs[3]) < 6, 'glide stops on the next cell');
  // hold right: one step now, repeats after the delay at a steady pace
  P._init_game(); P._frame(1000);
  P._step(1); P._hold(1);
  const path_ = [];
  for (let now = 1016; now <= 1640; now += 16) { P._frame(now); path_.push(capX()); }
  const jumps = path_.slice(1).map((x, i) => x - path_[i]);
  ok(Math.min(...jumps) > -4 && Math.max(...jumps) < 10, 'held walk never jumps or slides back (max ' + Math.max(...jumps).toFixed(1) + ' px/frame)');
  ok(path_.at(-1) - x0 > 3 * 32, 'holding a key keeps walking (' + ((path_.at(-1) - x0) / 32).toFixed(1) + ' cells)');
  P._hold(-1);
  P._frame(1800); const rest = capX(); P._frame(2200);
  ok(Math.abs(capX() - rest) < 1, 'releasing the key stops the walk');
  P._init_game(); P._frame(1000);
  P._step(1); P._hold(1); P._frame(1100); P._hold(-1); P._frame(1500);
  ok(Math.abs(capX() - x0 - 32) < 6, 'a short tap moves exactly one cell');
  P._init_game(); P._frame(1000);
  P._hold(1); P._init_game(); P._frame(1000); P._frame(1600);
  ok(Math.abs(capX() - x0) < 1, 'restart cancels a held key');
  P._init_game(); P._frame(1000);

  console.log('--- turning has no delay ---');
  // time from the key press until the farmer enters the new cell (shown by a defuse)
  const turnTime = (cell, setup) => {
    let now = 1000; P._init_game(); P._frame(now);
    const run = (to) => { while (now < to) P._frame(++now); };
    const press = setup(run);
    const s = P._get_score(), t0 = now;
    P._set_potato(cell[0], cell[1], 100000);
    press();
    while (P._get_score() === s && now - t0 < 600) run(now + 1);
    return now - t0;
  };
  const walkRight = (until) => (run) => { P._step(1); P._hold(1); run(until); return () => { P._step(0); P._hold(0); }; };
  const justAfter = turnTime([cx + 3, cy - 1], walkRight(1310));   // 20 ms after a held step
  ok(justAfter === 0, 'turning right after a walking step is instant (' + justAfter + ' ms)');
  const back = turnTime([cx + 4, cy - 2], (run) => {
    P._step(0); P._hold(0); run(1050); P._step(1); P._hold(1); run(1470);
    return () => P._hold(0);                                        // let go of right, still holding up
  });
  ok(back <= 100, 'falling back to a still-held key keeps the walking pace (' + back + ' ms)');
  P._init_game(); P._frame(1000);
  P._step(1); P._hold(1); P._frame(1050); P._step(0); P._hold(0); P._frame(1100); P._hold(-1); P._frame(1600);
  P._set_potato(cx + 1, cy - 2, 100000);
  const before = P._get_score(); P._step(0);
  ok(P._get_score() === before + 3, 'two quick overlapping taps move one cell each');

  console.log('--- tight controls ---');
  // fast taps: the farmer is never drawn more than one cell behind the rules
  P._init_game(); P._frame(1000);
  const home = capX();
  for (let i = 0; i < 6; i++) P._press(1010 + i * 30, 1);
  P._hold(-1); P._frame(1161);
  ok(capX() - home >= 5 * 32 - 4, 'rapid taps do not build up lag (' + ((capX() - home) / 32).toFixed(1) + ' of 6 cells drawn)');
  P._frame(1260);
  const arrived = capX(); P._frame(1600);
  ok(Math.abs(arrived - home - 6 * 32) < 6 && capX() === arrived, 'stops dead within one step of the last tap');
  // a held step falling due at the moment of a new press must not happen
  P._init_game(); P._frame(1000);
  P._press(1000, 1); P._frame(1150);                   // walking: next step due at 1250
  P._set_potato(cx + 3, cy, 100000);
  const s1 = P._get_score();
  P._press(1255, 0);
  ok(P._get_score() === s1, 'the old direction gets no extra step on a turn');
  P._init_game(); P._frame(1000);

  console.log('--- defuse: step onto a potato scores ---');
  const s0 = P._get_score();
  P._set_potato(cx + 1, cy, 100000);   // right of player, long fuse
  P._frame(1010);
  ok(hasColorNear(P, cx + 1, cy, [0xe8, 0xc5, 0x47]), 'spawned potato rendered (yellow, young fuse)');
  const s = P._step(1);                // step right
  ok(P._get_score() === s0 + 3, 'stepping onto a medium potato scores 3 (score ' + s0 + ' -> ' + P._get_score() + ')');
  P._frame(1160);                        // let the glide finish
  ok(hasColorNear(P, cx + 1, cy, CAP), 'farmer now on defused cell');

  console.log('--- explosion damage: in blast => lose heart ---');
  const h0 = P._get_hearts();
  P._set_potato(cx + 1, cy, 100);      // player stands on it
  P._frame(1185);                      // t = 25ms of its 100ms fuse... not yet
  ok(P._get_hearts() === h0, 'heart intact before fuse expiry');
  P._frame(1360);                      // past expiry
  ok(P._get_hearts() === h0 - 1, 'exploding on top of player costs a heart (' + h0 + ' -> ' + P._get_hearts() + ')');

  console.log('--- blast radius: outside 5x5 is safe ---');
  const h1 = P._get_hearts();
  P._set_potato(cx + 5, cy, 100);      // 5 cells away, chebyshev 5 > 2
  P._frame(1560);
  ok(P._get_hearts() === h1, 'potato 5 cells away does not damage player');

  const h2 = P._get_hearts();
  P._set_potato(cx + 1, cy, 100);      // adjacent - in blast
  P._frame(1760);
  ok(P._get_hearts() === h2 - 1, 'potato adjacent hits player (chebyshev <= 2)');

  console.log('--- chain: blast clears caught potatoes ---');
  P._init_game();
  P._frame(2000);                       // re-prime
  P._set_potato(6, 6, 100);            // A: explodes first, cheb 4 from player @(10,10)
  P._set_potato(8, 8, 5000);           // B: in A's blast, and would hit the player if it blew
  const h3 = P._get_hearts();
  P._frame(2150);                       // A explodes, B should be cleared
  P._frame(7000);                       // B's deadline (t=5000ms) passes
  ok(P._get_hearts() === h3, 'potato caught in another blast is destroyed, no double boom');

  console.log('--- death: 0 hearts ends game ---');
  P._init_game();
  P._frame(10000);
  P._set_potato(cx, cy, 50);
  P._frame(10100);
  P._set_potato(cx, cy, 50);
  P._frame(10200);
  P._set_potato(cx, cy, 50);
  P._frame(10300);
  ok(P._get_hearts() === 0, 'three blasts drain hearts to 0');
  ok(P._get_dead() === 1, 'game over after third blast');
  const sD = P._step(1);                // stepping after death should not crash / not score
  ok(P._get_dead() === 1 && P._get_score() === sD, 'still dead, no score after stepping');

  console.log('--- countdown ring ---');
  P._init_game();
  P._frame(1000);
  P._set_potato(3, 3, 3000);
  // count ring pixels still lit in the young-fuse yellow / mid-fuse orange / late red
  const lit = () => {
    let n = 0;
    for (let dy = -26; dy <= 26; dy++)
      for (let dx = -26; dx <= 26; dx++) {
        const r = Math.hypot(dx, dy);
        if (r < 23 || r > 26) continue;
        const p = String(pixelAt(P, 3 * 32 + 16 + dx, 3 * 32 + 16 + dy));
        if (p === '232,197,71' || p === '224,123,57' || p === '214,69,69') n++;
      }
    return n;
  };
  const arcs = [1001, 1750, 2500, 3250, 3900].map((t) => { P._frame(t); return lit(); });
  ok(arcs[0] > 200 && arcs.every((n, i) => i === 0 || n < arcs[i - 1]), 'ring drains as the fuse burns (' + arcs.join(' > ') + ')');
  ok(Math.abs(arcs[2] / arcs[0] - 0.5) < 0.08, 'half the ring is left at half time');

  console.log('--- particles ---');
  P._init_game();
  P._frame(20000);
  ok(P._get_particles() === 0, 'no particles after init');
  P._set_potato(2, 2, 50);
  P._frame(20100);
  ok(P._get_particles() > 150, 'explosion emits particles (' + P._get_particles() + ')');
  for (let t = 20116; t <= 22000; t += 16) P._frame(t);
  P._init_game();
  ok(P._get_particles() === 0, 'restart clears particles');
  P._frame(23000);
  P._set_potato(2, 2, 50);
  P._frame(23100);
  P._frame(26000);
  ok(P._get_particles() === 0, 'particles expire after a long gap');
  P._init_game();
  P._frame(27000);
  for (let i = 0; i < 9; i++) P._set_potato(i * 2 + 1, i % 2 ? 0 : 20, 50);   // edges, all at once
  for (let t = 27100; t < 27600; t += 16) P._frame(t);
  ok(P._get_particles() <= 3072, 'pool stays bounded with many edge blasts');
  P._set_reduced_motion(1);
  P._set_potato(0, 0, 10);
  P._frame(27650);
  ok(P._get_particles() > 0, 'reduced motion still renders blasts');
  P._set_reduced_motion(0);

  console.log('--- robustness: bad input ---');
  P._init_game();
  P._frame(-500);                       // negative clock must prime once only
  P._set_potato(0, 0, 100);
  P._frame(-300);
  ok(P._get_particles() > 0, 'negative timestamps advance the game');
  P._init_game();
  P._frame(1000);
  P._frame(NaN); P._frame(Infinity); P._frame(500);
  ok(P._get_dead() === 0 && P._get_hearts() === 3, 'NaN / Infinity / backwards time are harmless');
  P._set_potato(-1, 5, 100); P._set_potato(21, 5, 100); P._set_potato(1e9, -1e9, 100);
  P._frame(1200);
  ok(P._get_particles() === 0, 'off-grid potatoes are rejected');
  P._set_potato(0, 0, NaN); P._set_potato(1, 20, -5); P._set_potato(20, 0, 0);
  P._frame(1300);
  ok(P._get_particles() > 0, 'bad fuse values explode promptly instead of crashing');
  const t0 = Date.now();
  P._frame(1e15);
  P._frame(1e300);
  ok(Date.now() - t0 < 1000, 'huge time jump returns quickly');
  P._init_game();
  P._frame(1000);
  P._step(99); P._step(-1);
  P._set_potato(cx + 1, cy, 100000);
  P._step(1);
  ok(P._get_score() === 3, 'invalid directions are ignored');
  P._init_game();
  P._frame(1000);
  for (let i = 0; i < 30; i++) { P._frame(1100 + i * 60); P._step(3); }
  P._set_potato(1, cy, 100000);
  P._frame(4000);
  P._step(3);                           // x is 0 if the wall held, so nothing at x=1
  ok(P._get_score() === 0, 'walls block movement');

  console.log('--- one-minute round ---');
  P._init_game();
  ok(P._get_time_left() === 60000 && P._get_over() === 0, 'clock shows 1:00 before the first frame');
  P._frame(1000);
  P._frame(31000);
  ok(P._get_time_left() === 30000, 'half the time left at 30 s');
  P._init_game(); P._frame(1000); P._frame(60900);
  P._init_game(); P._frame(1000);
  // a potato defused just before the buzzer counts, one stepped on after it does not
  P._frame(60990);
  const alive = !P._get_dead();
  P._set_potato(cx + 1, cy, 100000); P._step(1);
  const atBuzzer = P._get_score();
  P._frame(61000);
  ok(alive && P._get_over() === 1 && P._get_time_left() === 0, 'round ends at 60 s');
  P._set_potato(cx + 2, cy, 100000);
  P._step(1); P._press(61100, 1); P._frame(62000);
  ok(atBuzzer === 3 && P._get_score() === 3, 'no moving or scoring after time is up');
  P._init_game();
  ok(P._get_over() === 0 && P._get_time_left() === 60000, 'restart resets the clock');
  P._frame(1000); P._press(1000, 1); P._frame(60900); P._frame(70000);
  const held0 = P._get_particles();
  ok(P._get_over() === 1 && P._get_potatoes() === 0 && held0 === 0, 'field is cleared at the buzzer, nothing blows later');
  // a fuse that ran out before the buzzer still explodes, even if the frame is late
  P._init_game(); P._frame(1000); P._frame(60000);
  P._set_potato(0, 0, 500);                          // due at 59.5 s
  P._frame(65000);
  ok(P._get_particles() > 0, 'late frame still explodes fuses that expired in time');

  console.log('--- potato sizes ---');
  const SIZES = ['small', 'medium', 'big'], WORTH = [1, 3, 5], REACH = [1, 2, 3], bursts = [];
  for (let z = 0; z < 3; z++) {
    P._init_game(); P._frame(1000);
    P._set_sized_potato(cx + 1, cy, 100000, z); P._step(1);
    ok(P._get_score() === WORTH[z] && P._get_defused() === 1, SIZES[z] + ' potato is worth ' + WORTH[z]);
    const hit = (dist) => {
      P._init_game(); P._frame(1000);
      P._set_sized_potato(cx + dist, cy + dist, 50, z); P._frame(1100);
      return P._get_hearts() < 3;
    };
    ok(hit(REACH[z]) && !hit(REACH[z] + 1), SIZES[z] + ' blast reaches exactly ' + REACH[z] + ' cells');
    P._init_game(); P._frame(1000);
    P._set_sized_potato(3, 3, 50, z); P._frame(1100);
    bursts.push(P._get_particles());
  }
  ok(bursts[0] < bursts[1] && bursts[1] < bursts[2], 'bigger potato, bigger explosion (' + bursts.join(' < ') + ' particles)');
  // how much of the board the ring covers, and how far the screen is thrown about
  const ringArea = (z) => {
    P._init_game(); P._frame(1000);
    const plain = P.HEAPU8.slice(P._game_buf(), P._game_buf() + 672 * 672 * 4);
    P._set_sized_potato(4, 4, 100000, z); P._frame(1001);
    const now = P._game_buf(); let n = 0;
    for (let i = 0; i < plain.length; i += 4) if (P.HEAPU8[now + i] !== plain[i]) n++;
    return n;
  };
  const areas = [0, 1, 2].map(ringArea);
  ok(areas[0] < areas[1] && areas[1] < areas[2], 'bigger potatoes look bigger (' + areas.join(' < ') + ' px)');
  P._init_game(); P._frame(1000);
  P._set_sized_potato(1, 1, 100, 3); P._set_sized_potato(1, 1, 100, -1);
  ok(P._get_potatoes() === 0, 'unknown sizes are rejected');
  const seenSizes = [0, 0, 0];
  for (let game = 0; game < 30; game++) {
    P._init_game(); const t0 = 1000 + game * 61000; P._frame(t0); P._frame(t0 + 800);
    for (let i = 0; i < 20; i++) if (P._potato_size(i) >= 0) seenSizes[P._potato_size(i)]++;
  }
  ok(seenSizes.every((n) => n > 0) && seenSizes[0] > seenSizes[2], 'all sizes spawn, big ones rarest (' + seenSizes.join(' / ') + ')');

  console.log('--- more potatoes as time goes on ---');
  const times = [0, 15000, 30000, 45000, 60000];
  const rising = (f, dir) => times.map(f).every((v, i, a) => i === 0 || dir * (v - a[i - 1]) > 0);
  ok(rising((t) => P._potato_cap(t), 1), 'more allowed on the field (' + times.map((t) => P._potato_cap(t)).join(' ') + ')');
  ok(rising((t) => P._spawn_gap(t), -1), 'they arrive faster (' + times.map((t) => P._spawn_gap(t)).join(' ') + ' ms)');
  ok(rising((t) => P._fuse_length(t), -1), 'fuses get shorter (' + times.map((t) => P._fuse_length(t)).join(' ') + ' ms)');
  let early = 0, late = 0, overCap = 0;
  for (let game = 0; game < 40; game++) {
    P._init_game(); const t0 = 1000 + game * 61000; P._frame(t0);
    for (let t = 100; t < 60000 && !P._get_over(); t += 100) {
      P._frame(t0 + t);
      const n = P._get_potatoes();
      if (n > P._potato_cap(t)) overCap++;
      if (t < 10000) early = Math.max(early, n); else if (t > 45000) late = Math.max(late, n);
    }
  }
  ok(overCap === 0 && late >= early + 4, 'busier at the end than the start (peak ' + early + ' -> ' + late + ')');

  console.log('--- top ten ---');
  const createScoreboard = require(path.join(__dirname, 'scores.js'));
  const store = (data = {}) => ({ data, getItem: (k) => (k in data ? data[k] : null), setItem: (k, v) => { data[k] = String(v); } });
  const disk = store(), board = createScoreboard(disk);
  ok(board.list().length === 0 && !board.qualifies(0) && board.qualifies(1), 'empty board takes any score above zero');
  for (let i = 1; i <= 12; i++) board.add('p' + i, i);
  const top = board.list();
  ok(top.length === 10 && top[0].score === 12 && top[9].score === 3, 'keeps the best ten in order');
  ok(!board.qualifies(3) && board.qualifies(4) && board.add('low', 2) === -1, 'must beat tenth place');
  ok(board.add('tie', 12) === 1, 'a tie ranks below the earlier score');
  ok(createScoreboard(disk).list()[1].name === 'TIE' && createScoreboard(disk).lastName() === 'TIE', 'survives a reload');
  board.add('  a very   long name ', 99); board.add('   ', 98);
  ok(board.list()[0].name === 'A VERY L' && board.list()[1].name === 'FARMER', 'names are trimmed, capped and defaulted');
  for (const bad of ['not json', '{}', 'null', '[1,null,{"name":5,"score":3},{"name":"x","score":"9"}]']) {
    const b = createScoreboard(store({ 'exploding-potatoes.top10.v1': bad }));
    if (b.list().length !== 0 || b.add('ok', 5) !== 0) ok(false, 'damaged data: ' + bad);
  }
  ok(true, 'damaged saved data is ignored');
  const broken = { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } };
  const full = { ...store({ 'exploding-potatoes.top10.v1': '[{"name":"OLD","score":1}]' }), setItem() { throw new Error('quota'); } };
  for (const [label, s] of [['no storage', null], ['blocked storage', broken], ['full storage', full]]) {
    const b = createScoreboard(s); b.add('me', 7);
    ok(b.list()[0].score === 7, label + ': still ranks for this visit');
  }

  console.log('--- auto-spawn + soak ---');
  P._init_game();
  P._frame(30000);
  for (let i = 0; i < 40; i++) { P._frame(30000 + i * 60); }
  for (let i = 0; i < 3000; i++) {
    P._frame(32400 + i * 16);
    if (i % 5 === 0) P._step((i / 5 * 7 + (i >> 8)) % 4);
    if (P._get_dead()) P._init_game();
  }
  ok(true, 'long soak with spawns/fuses/explosions, no crash (hearts now ' + P._get_hearts() + ', dead=' + P._get_dead() + ')');

  console.log('');
  if (failures) { console.log(failures + ' FAILURE(S)'); process.exit(1); }
  console.log('ALL TESTS PASSED');
}

main().catch((e) => { console.error('HARNESS ERROR:', e); process.exit(2); });
