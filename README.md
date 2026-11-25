# Exploding Potatoes

A tiny survival game written in C, compiled to WebAssembly, rendered as raw
pixels into a canvas. You are the farmer. Fused potatoes spawn
around the grid, each with a countdown ring: step onto one to defuse it, or stay out of its blast
(you have 3 hearts). A round lasts one minute and potatoes come faster as the
clock runs: score high enough to make the top ten. Restart with **R**.

![controls](https://img.shields.io/badge/controls-arrows%20%C2%B7%20R-2ea043)

## Potatoes

| Size   | Points | Blast | Share |
| ------ | ------ | ----- | ----- |
| Small  | 1      | 3×3   | 50%   |
| Medium | 3      | 5×5   | 35%   |
| Big    | 5      | 7×7   | 15%   |

## Play

Serve the directory and open it in a browser (opening `index.html` straight from
disk fails in browsers that block loading `.wasm` over `file://`):

```sh
python3 -m http.server 8080
# then visit http://localhost:8080
```

Needs a browser with WebAssembly SIMD (Chrome 91+, Firefox 89+, Safari 16.4+).

- **Arrows / WASD** — hold to walk; the newest key wins

Explosions knock you away, even on a near miss.
- **R** — restart

The clock starts on your first move. The top ten is saved in that browser only.

Screen shake and flash are turned off when the system asks for reduced motion.

## Build

Requires [emsdk](https://emsdk.dev/):

```sh
source /path/to/emsdk/emsdk_env.sh
./build.sh
```

The committed `potato.js` / `potato.wasm` are current builds — commit them if
you rebuild.

The potato photo, farmer sprite sheet and field tiles are baked into `src/sprites.h`. After changing anything in
`assets/`, regenerate it (needs [Pillow](https://python-pillow.org/)):

```sh
python3 tools/make_sprites.py && ./build.sh
```

## Test

The Node harness loads the compiled module the same way the browser does and
checks rendering, movement, blasts, particles, bad input and restart:

```sh
node test_node.js
# ALL TESTS PASSED
```

## Layout

| File           | What it is                                          |
| -------------- | --------------------------------------------------- |
| `src/potato.c` | Game in C: logic, spawner, collisions, particles, pixel render |
| `src/sprites.h` | Generated sprite pixels                            |
| `assets/`      | Potato, farmer and field art, [credits](assets/CREDITS.md) |
| `tools/make_sprites.py` | `assets/` → `src/sprites.h`                |
| `build.sh`     | Emscripten build                                    |
| `scores.js`    | Top-ten ranking                                     |
| `potato.js`    | Emscripten glue (MODULARIZE, factory `createPotato`) |
| `potato.wasm`  | Compiled WASM module                                 |
| `index.html`   | Browser shell: canvas, timer, screens, input        |
| `test_node.js` | Node test harness for the same WASM module           |

## Design notes

- The engine owns the canvas: it renders straight into a WebAssembly memory
  buffer (RGBA) that JS blits with `putImageData`. Drawing is lazy: state changes
  mark the picture stale and `_game_buf()` draws it.
- The farmer moves freely and stops as soon as the key is released. Potatoes sit
  on a grid: walking close defuses one, and blasts are judged cell by cell.
- Time is wall-clock (`performance.now()`), so the game advances in real time
  even in a throttled background tab; input events force an immediate
  render+HUD refresh so a stale frame can never be left on screen.
