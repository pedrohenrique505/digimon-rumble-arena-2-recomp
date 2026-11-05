# Digimon Rumble Arena 2 — Static Recompilation

Recompilação estática de **Digimon Rumble Arena 2** (PS2, NTSC-U, `SLUS-21067`) para um executável nativo de PC. Por enquanto o alvo é só Linux.

O código MIPS R5900 do Emotion Engine é traduzido para C++ e ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). Não é um emulador: o jogo vira um programa nativo.

> **Este repositório não contém nenhum arquivo do jogo.** Para usar, você precisa da sua própria cópia legal da ISO.

## Estado

**O jogo roda o laço principal.** O ELF inteiro recompila com o [PS2Recomp](https://github.com/ran-j/ps2recomp) (78.202 funções, 0 instruções não tratadas). Os 10 módulos IOP do jogo rodam no IOP emulado, o driver próprio do jogo lê o disco, e o jogo manda DMA de render todo frame. O bloqueio atual é o pipeline VIF1/VU1 → GS: o fim de frame ainda não chega ao GS.

Correções genéricas que fizemos no runtime/IOP do PS2Recomp ficam em [`patches/ps2recomp/`](patches/ps2recomp/).

Veja [`progression/`](progression/) para o diário de progresso.

## Estrutura

- `docs/`: pesquisa e documentação técnica ([ferramentas](docs/ferramentas.md))
- `progression/`: registro do progresso do projeto, por data
- `runtime/overrides/`: correções específicas do jogo (game overrides do PS2Recomp)
- `patches/ps2recomp/`: correções genéricas aplicadas ao clone do PS2Recomp (candidatas a PR)
- `tools/`: export do Ghidra, bindings do SDK e aplicação dos patches
- `CMakeLists.txt`: build do executável (runtime do PS2Recomp + código gerado + overrides)

## Build (Linux)

Os passos completos estão no [`CLAUDE.md`](CLAUDE.md#comandos). Em resumo:

1. Extraia o disco para `game/` (a ISO não fica no repositório).
2. Exporte a function map do Ghidra (`tools/ghidra_export.sh`) e recompile com o `ps2_recomp`, gerando `generated/`.
3. Rode o `ps2_analyzer` e o `tools/gen_sdk_bindings.py`, e aplique os patches com `tools/apply_upstream_patches.sh`.
4. `cmake -S . -B build -G Ninja ...` e `cmake --build build --target dra2`.

## Ferramentas

Ghidra 12.1.2 + [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded), [PS2Recomp](https://github.com/ran-j/ps2recomp), PCSX2 (referência). Detalhes em [`docs/ferramentas.md`](docs/ferramentas.md).
