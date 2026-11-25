#!/bin/sh
# Rebuild potato.js / potato.wasm. Needs emcc on PATH (source emsdk_env.sh).
set -eu
cd "$(dirname "$0")"
command -v emcc >/dev/null || { echo "emcc not found: source /path/to/emsdk/emsdk_env.sh" >&2; exit 1; }
emcc src/potato.c -O3 -msimd128 -Wall -Wextra -o potato.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createPotato \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8
