#!/usr/bin/env bash
# Aplica os patches de patches/ps2recomp/ no clone external/PS2Recomp.
# Idempotente: pula patches já aplicados. São correções genéricas do runtime/IOP que
# não dá para fazer por game override. São mantidas por nós (não enviamos para o upstream).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UP="${PS2RECOMP_DIR:-$ROOT/external/PS2Recomp}"
for p in "$ROOT"/patches/ps2recomp/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$UP" apply --reverse --check "$p" 2>/dev/null; then
    echo "já aplicado: $(basename "$p")"
  else
    git -C "$UP" apply "$p" && echo "aplicado:    $(basename "$p")"
  fi
done
