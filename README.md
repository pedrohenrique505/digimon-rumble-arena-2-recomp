# Digimon Rumble Arena 2 — Static Recompilation

Recompilação estática de **Digimon Rumble Arena 2** (PS2, NTSC-U, `SLUS-21067`) para um executável nativo de PC. Por enquanto o alvo é só Linux.

O código MIPS R5900 do Emotion Engine é traduzido para C++ e ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). Não é um emulador: o jogo vira um programa nativo.

> **Este repositório não contém nenhum arquivo do jogo.** Para usar, você precisa da sua própria cópia legal da ISO.

## Estado

**O jogo chega ao menu inicial e permite navegar entre opções.** O ELF inteiro recompila com o [PS2Recomp](https://github.com/ran-j/ps2recomp) (78.202 funções, 0 instruções não tratadas). Os 10 módulos IOP rodam, o disco é lido e o pipeline VIF1/VU1 → GS renderiza a abertura. Foram corrigidos o alinhamento LQ/SQ e a paginação do memory card, permitindo concluir o autoload. O patch 0013 corrige a perda de checkpoints recursivos no dispatcher: `lua/menu` agora compila e cria cinco opções, confirmadas na RAM e no vetor C++. Os 35 testes de expansão passaram, e a navegação pelo teclado mudou a seleção de 1 para 2. Restam defeitos visuais nas transições do carrossel; a entrada em uma partida ainda não foi testada.

**Desempenho ainda baixo.** A primeira rodada de otimização usou REA/GDB e
Ghidra para medir a abertura. O patch 0014 melhorou a vazão do benchmark bilinear
em 16%, preservando a VRAM; o contador de renderização do jogo avançou de cerca
de 7,8 para 8,1–8,2 quadros/s. Isso não mede uma partida. Veja o
[plano de desempenho e comandos de medição](docs/desempenho.md) e o
[registro das duas primeiras etapas](progression/2026-11-07-desempenho-etapas-1-2.md).

Bugs encontrados no runtime/IOP do PS2Recomp são corrigidos por nós e mantidos como patches em [`patches/ps2recomp/`](patches/ps2recomp/).

Veja o [estado e próximo passo](progression/2026-11-07-checkpoint-recursivo-corrigido.md) e o [diário completo](progression/) para os detalhes.

## Estrutura

- `docs/`: pesquisa e documentação técnica ([ferramentas](docs/ferramentas.md))
- `progression/`: registro do progresso do projeto, por data
- `runtime/overrides/`: correções específicas do jogo (game overrides do PS2Recomp)
- `patches/ps2recomp/`: nossas correções no runtime/IOP do PS2Recomp, aplicadas ao clone
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
