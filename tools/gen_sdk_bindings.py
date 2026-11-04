#!/usr/bin/env python3
"""Gera runtime/overrides/dra2_sdk_bindings.inc a partir do TOML do ps2_analyzer.

O ps2_analyzer (PS2Recomp) reconhece funções do SDK da Sony dentro do ELF stripped
com um banco de assinaturas embutido (sce-symbol-scanner). Cada entrada
"nome@0xENDEREÇO" de [general].stubs tem um handler HLE no runtime. O override
liga esses endereços aos handlers em tempo de execução (bindAddressHandler), sem
precisar recompilar o código gerado.

Uso:
  external/PS2Recomp/out/build/ps2xAnalyzer/ps2_analyzer game/SLUS_210.67 ghidra/analyzer/analyzer.toml
  python3 tools/gen_sdk_bindings.py [ghidra/analyzer/analyzer.toml]
"""
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "ghidra/analyzer/analyzer.toml"
OUT = ROOT / "runtime/overrides/dra2_sdk_bindings.inc"

# Não ligar: substituídos por implementação nossa, ou só TODO no runtime.
EXCLUDE = {
    "write",  # dra2_overrides.cpp: ttyWrite (DECI2 trava sem devkit)
    "sceTtyWrite", "sceTtyRead", "sceTtyInit", "sceTtyHandler",
    "sceDeci2Open", "sceDeci2ReqSend", "sceDeci2Poll", "sceDeci2ExRecv", "sceDeci2ExSend",
}

stubs = tomllib.loads(SRC.read_text())["general"]["stubs"]
entries, skipped = [], []
for item in stubs:
    name, addr = item.split("@")
    (skipped if name in EXCLUDE else entries).append((int(addr, 16), name))
entries.sort()

lines = [
    "// GERADO por tools/gen_sdk_bindings.py a partir do ps2_analyzer. Não edite à mão.",
    f"// {len(entries)} bindings; excluídos: {', '.join(sorted(n for _, n in skipped))}",
]
lines += [f'{{0x{a:08X}u, "{n}"}},' for a, n in entries]
OUT.write_text("\n".join(lines) + "\n")
print(f"{OUT.relative_to(ROOT)}: {len(entries)} bindings ({len(skipped)} excluídos)")
