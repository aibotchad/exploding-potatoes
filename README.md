# Exploding Potatoes

A tiny survival game written in C, compiled to WebAssembly, rendered as raw
pixels into a canvas. Potatoes spawn around your disc with fuses; dodge the
blast radius (or take the hit — you have 3 hearts). Survive, rack up score,
restart with **R**.

![controls](https://img.shields.io/badge/controls-arrows%20%C2%B7%20R-2ea043)

## Play

Open `index.html` in a browser (or serve the directory):

```sh
python3 -m http.server 8080
# then visit http://localhost:8080
```

- **Arrows / WASD** — move one grid cell
- **R** — restart after a game over

## Build

Requires [emsdk](https://emsdk.dev/):

```sh
source /path/to/emsdk/emsdk_env.sh
emcc src/potato.c -O2 -o index.js \
  --use-port -s MODULARIZE=1 -s EXPORT_NAME=createPotato \
  -s EXPORTED_FUNCTIONS='["_init_game","_step","_frame","_game_w","_game_h","_game_buf"]'
```

The committed `potato.js` / `potato.wasm` are current builds — commit them if
you rebuild.

## Test

The Node harness loads the compiled module the same way the browser does
(shared memory-backed canvas) and checks rendering, movement, collision,
and restart:

```sh
node test_node.js
# 24/24 PASS
```

## Layout

| File           | What it is                                          |
| -------------- | --------------------------------------------------- |
| `src/potato.c` | Game in C: logic, spawner, collisions, pixel render |
| `potato.js`    | Emscripten glue (MODULARIZE, factory `createPotato`) |
| `potato.wasm`  | Compiled WASM module                                 |
| `index.html`   | Browser shell: canvas blit, HUD, input, kickers     |
| `test_node.js` | Node test harness for the same WASM module           |

## Design notes

- The engine owns the canvas: it renders straight into a WebAssembly memory
  buffer (`_game_buf`, RGBA) that JS blits with `putImageData`.
- Time is wall-clock (`performance.now()`), so the game advances in real time
  even in a throttled background tab; input events force an immediate
  render+HUD refresh so a stale frame can never be left on screen.
