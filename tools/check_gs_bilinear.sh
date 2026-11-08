#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.cache/gs-bilinear}"
mkdir -p "$OUT"
RT="$ROOT/external/PS2Recomp/ps2xRuntime"
"${CXX:-c++}" -std=c++20 -O2 -DNDEBUG -pthread \
  -DPS2_RUNTIME_LOGS=0 -DAGRESSIVE_LOGS=0 \
  -I"$RT/include" -I"$RT/src/lib" \
  "$ROOT/tools/test_gs_bilinear.cpp" "$RT/src/lib/gs/ps2_gs_memory.cpp" \
  -o "$OUT/test_gs_bilinear"
"$OUT/test_gs_bilinear"
