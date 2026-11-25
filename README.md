# Exploding Potatoes

A tiny survival game written in C, compiled to WebAssembly, rendered as raw
pixels into a canvas. You are the farmer. Fused potatoes spawn
around the grid, each with a countdown ring: step onto one to defuse it (+1), or stay out of its 5×5 blast
(you have 3 hearts). Survive, rack up score, restart with **R**.

![controls](https://img.shields.io/badge/controls-arrows%20%C2%B7%20R-2ea043)

## Play

Serve the directory and open it in a browser (opening `index.html` straight from
disk fails in browsers that block loading `.wasm` over `file://`):

```sh
python3 -m http.server 8080
# then visit http://localhost:8080
```

- **Arrows / WASD** — tap to move one cell, hold to keep walking
- **R** — restart

Screen shake and flash are turned off when the system asks for reduced motion.

## Build

Requires [emsdk](https://emsdk.dev/):

```sh
source /path/to/emsdk/emsdk_env.sh
./build.sh
```

The committed `potato.js` / `potato.wasm` are current builds — commit them if
you rebuild.

The potato photo and farmer sprite sheet are baked into `src/sprites.h`. After changing anything in
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
| `assets/`      | Potato photo, farmer sheet, [credits](assets/CREDITS.md) |
| `tools/make_sprites.py` | `assets/` → `src/sprites.h`                |
| `build.sh`     | Emscripten build                                    |
| `potato.js`    | Emscripten glue (MODULARIZE, factory `createPotato`) |
| `potato.wasm`  | Compiled WASM module                                 |
| `index.html`   | Browser shell: canvas blit, HUD, input, watchdog    |
| `test_node.js` | Node test harness for the same WASM module           |

## Design notes

- The engine owns the canvas: it renders straight into a WebAssembly memory
  buffer (`_game_buf`, RGBA) that JS blits with `putImageData`.
- Time is wall-clock (`performance.now()`), so the game advances in real time
  even in a throttled background tab; input events force an immediate
  render+HUD refresh so a stale frame can never be left on screen.
