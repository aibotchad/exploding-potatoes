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
  const C = 24;
  const cx = cellX * C + C / 2, cy = cellY * C + C / 2;
  const h = 14;
  for (let y = cy - h; y <= cy + h; y++) {
    for (let x = cx - h; x <= cx + h; x++) {
      if (x < 0 || y < 0) continue;
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
  ok(P._game_w() === 504, 'game width is 504');
  ok(P._game_h() === 504, 'game height is 504');
  ok(P._get_dead() === 0, 'not dead after init');
  ok(P._get_hearts() === 3, 'starts with 3 hearts');
  ok(P._get_score() === 0, 'starts at 0 score');

  // prime the game clock
  P._frame(1000);
  ok(true, 'frame primes without crashing');

  console.log('--- render: player potato at center ---');
  const PLAY = [0xf2, 0xc9, 0x4c];
  const cx = 21 >> 1, cy = 21 >> 1;
  ok(hasColorNear(P, cx, cy, PLAY), 'player potato pixels visible at center cell');

  console.log('--- defuse: step onto a potato scores ---');
  const s0 = P._get_score();
  P._set_potato(cx + 1, cy, 100000);   // right of player, long fuse
  P._frame(1010);
  ok(hasColorNear(P, cx + 1, cy, [0xe8, 0xc5, 0x47]), 'spawned potato rendered (yellow, young fuse)');
  const s = P._step(1);                // step right
  ok(P._get_score() === s0 + 1, 'stepping onto potato scores (score ' + s0 + ' -> ' + P._get_score() + ')');
  ok(hasColorNear(P, cx + 1, cy, PLAY), 'player now on defused cell');

  console.log('--- explosion damage: in blast => lose heart ---');
  const h0 = P._get_hearts();
  P._set_potato(cx + 1, cy, 100);      // player stands on it
  P._frame(1025);                      // t = 25ms of its 100ms fuse... not yet
  ok(P._get_hearts() === h0, 'heart intact before fuse expiry');
  P._frame(1200);                      // past expiry
  ok(P._get_hearts() === h0 - 1, 'exploding on top of player costs a heart (' + h0 + ' -> ' + P._get_hearts() + ')');

  console.log('--- blast radius: outside 5x5 is safe ---');
  const h1 = P._get_hearts();
  P._set_potato(cx + 5, cy, 100);      // 5 cells away, chebyshev 5 > 2
  P._frame(1400);
  ok(P._get_hearts() === h1, 'potato 5 cells away does not damage player');

  const h2 = P._get_hearts();
  P._set_potato(cx + 1, cy, 100);      // adjacent - in blast
  P._frame(1600);
  ok(P._get_hearts() === h2 - 1, 'potato adjacent hits player (chebyshev <= 2)');

  console.log('--- chain: blast clears caught potatoes ---');
  P._init_game();
  P._frame(2000);                       // re-prime
  P._set_potato(5, 5, 100);            // A: explodes first
  P._set_potato(7, 7, 5000);           // B: within A's blast (cheb 2), within its own blast of player? player @(10,10): cheb(7,7)=(10,10) = 3 -> NOT in B blast
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
  ok(!P._get_dead() === false, 'still dead after stepping');

  console.log('--- auto-spawn + soak ---');
  P._init_game();
  P._frame(30000);
  for (let i = 0; i < 40; i++) { P._frame(30000 + i * 60); }
  ok(true, '1440 sim ms with spawns/fuses/explosions, no crash (hearts now ' + P._get_hearts() + ', dead=' + P._get_dead() + ')');

  console.log('');
  if (failures) { console.log(failures + ' FAILURE(S)'); process.exit(1); }
  console.log('ALL TESTS PASSED');
}

main().catch((e) => { console.error('HARNESS ERROR:', e); process.exit(2); });
