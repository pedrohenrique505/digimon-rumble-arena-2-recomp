#!/usr/bin/env bash
# Standalone proof: pin upstream and fetch only dependencies used by the tested build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${1:-$ROOT/.cache/parallel-gs-source}"
BUILD="${2:-$ROOT/.cache/gs-replay-build}"
REVISION=cc6184af7e0c03da603045ca371ffa5dae9b0655
if [[ ! -d "$SOURCE/.git" ]]; then
  mkdir -p "$SOURCE"
  git -C "$SOURCE" init
  git -C "$SOURCE" remote add origin https://github.com/Arntzen-software/parallel-gs.git
  git -C "$SOURCE" fetch --depth 1 origin "$REVISION"
  git -C "$SOURCE" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$SOURCE" rev-parse HEAD)" == "$REVISION" ]] || { echo 'Unexpected paraLLEl-GS revision' >&2; exit 1; }
git -C "$SOURCE" diff --quiet --ignore-submodules=all || { echo 'paraLLEl-GS has local changes' >&2; exit 1; }
git -C "$SOURCE" submodule update --init --depth 1 Granite
git -C "$SOURCE/Granite" submodule update --init --depth 1 \
  third_party/khronos/vulkan-headers third_party/volk third_party/rapidjson \
  third_party/stb/stb third_party/spirv-headers
cmake -S "$ROOT/tools/gs_replay" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPARALLEL_GS_SOURCE="$SOURCE"
cmake --build "$BUILD" --target dra2-gs-replay dra2-gs-replay-vulkan -j2
