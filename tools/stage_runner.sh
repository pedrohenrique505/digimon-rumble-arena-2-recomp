#!/usr/bin/env bash
# Liga (symlink) a saída do ps2_recomp em generated/ e os overrides de runtime/overrides/ dentro do ps2xRuntime do PS2Recomp,
# no layout que o CMake do ps2EntryRunner espera:
#   *.cpp -> ps2xRuntime/src/runner/   |   *.h -> ps2xRuntime/include/
# Arquivos do upstream não são apagados; só os links criados por este script.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN="$ROOT/generated"
RT="${PS2RECOMP_DIR:-$ROOT/external/PS2Recomp}/ps2xRuntime"

[ -d "$GEN" ] || { echo "faltando $GEN: rode o ps2_recomp antes" >&2; exit 1; }

OVR="$ROOT/runtime/overrides"

# Remove links antigos que apontam para generated/ ou runtime/overrides/
find "$RT/src/runner" "$RT/include" -maxdepth 1 -type l \( -lname "$GEN/*" -o -lname "$OVR/*" \) -delete

# O register_functions.cpp do upstream é um placeholder; o gerado o substitui.
if [ -f "$RT/src/runner/register_functions.cpp" ] && [ ! -L "$RT/src/runner/register_functions.cpp" ]; then
  mv "$RT/src/runner/register_functions.cpp" "$RT/src/runner/register_functions.cpp.upstream"
fi

find "$GEN" -maxdepth 1 -name '*.cpp' -exec ln -sf -t "$RT/src/runner" {} +
find "$GEN" -maxdepth 1 -name '*.h'   -exec ln -sf -t "$RT/include" {} +
# Nossas correções por jogo (game overrides): código do projeto, versionado.
# Prefixo zz_: o unity build agrupa em lotes de 32 por ordem alfabética. Um arquivo
# no meio da lista deslocaria todos os lotes e forçaria recompilar ~2.400 TUs.
for f in "$OVR"/*.cpp; do
  [ -e "$f" ] && ln -sf "$f" "$RT/src/runner/zz_$(basename "$f")"
done

echo "runner: $(find "$RT/src/runner" -maxdepth 1 -type l | wc -l) cpp | include: $(find "$RT/include" -maxdepth 1 -type l | wc -l) h"
