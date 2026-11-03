#!/usr/bin/env bash
# Liga (symlink) a saída do ps2_recomp em generated/ dentro do ps2xRuntime do PS2Recomp,
# no layout que o CMake do ps2EntryRunner espera:
#   *.cpp -> ps2xRuntime/src/runner/   |   *.h -> ps2xRuntime/include/
# Arquivos do upstream não são apagados; só os links criados por este script.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN="$ROOT/generated"
RT="${PS2RECOMP_DIR:-$ROOT/external/PS2Recomp}/ps2xRuntime"

[ -d "$GEN" ] || { echo "faltando $GEN: rode o ps2_recomp antes" >&2; exit 1; }

# Remove links antigos que apontam para generated/
find "$RT/src/runner" "$RT/include" -maxdepth 1 -type l -lname "$GEN/*" -delete

# O register_functions.cpp do upstream é um placeholder; o gerado o substitui.
if [ -f "$RT/src/runner/register_functions.cpp" ] && [ ! -L "$RT/src/runner/register_functions.cpp" ]; then
  mv "$RT/src/runner/register_functions.cpp" "$RT/src/runner/register_functions.cpp.upstream"
fi

find "$GEN" -maxdepth 1 -name '*.cpp' -exec ln -sf -t "$RT/src/runner" {} +
find "$GEN" -maxdepth 1 -name '*.h'   -exec ln -sf -t "$RT/include" {} +

echo "runner: $(find "$RT/src/runner" -maxdepth 1 -type l | wc -l) cpp | include: $(find "$RT/include" -maxdepth 1 -type l | wc -l) h"
