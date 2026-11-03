#!/usr/bin/env bash
# Importa e analisa o ELF no Ghidra headless (projeto separado do da GUI)
# e roda o ExportPS2Functions.java do PS2Recomp, gerando TOML + CSV.
#
# Uso: tools/ghidra_export.sh [caminho/do/SLUS_210.67]
# Requer: Ghidra 12.1.2 em /opt/ghidra, plugin ghidra-emotionengine-reloaded,
#         ExportPS2Functions.java em ~/ghidra_scripts (ou EXPORT_SCRIPT_DIR)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$ROOT/game/SLUS_210.67}"
GHIDRA="${GHIDRA_HOME:-/opt/ghidra}"
SCRIPT_DIR="${EXPORT_SCRIPT_DIR:-$HOME/ghidra_scripts}"
PROJ_DIR="$ROOT/ghidra/headless"
OUT_DIR="$ROOT/ghidra/export"
NAME="$(basename "$ELF")"

mkdir -p "$PROJ_DIR" "$OUT_DIR"

# No modo headless, cada askFile() do script consome um argumento, em ordem:
# 1) arquivo TOML de saída, 2) arquivo CSV de saída.
SCRIPT=(-scriptPath "$SCRIPT_DIR" -postScript ExportPS2Functions.java
        "$OUT_DIR/$NAME.toml" "$OUT_DIR/$NAME.csv")

if [ -d "$PROJ_DIR/DRA2_headless.rep" ]; then
  # Projeto já existe: só roda o script sobre o programa já analisado
  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" DRA2_headless \
    -process "$NAME" -noanalysis "${SCRIPT[@]}"
else
  "$GHIDRA/support/analyzeHeadless" "$PROJ_DIR" DRA2_headless \
    -import "$ELF" -processor "r5900:LE:32:default" "${SCRIPT[@]}"
fi

echo "Saída em: $OUT_DIR"
ls -la "$OUT_DIR"
