# Digimon Rumble Arena 2 — Static Recompilation

Recompilação estática de **Digimon Rumble Arena 2** (PS2, NTSC-U, `SLUS-21067`) para um executável nativo de PC. Por enquanto o alvo é só Linux.

O código MIPS R5900 do Emotion Engine é traduzido para C++ e ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). Não é um emulador: o jogo vira um programa nativo.

> **Este repositório não contém nenhum arquivo do jogo.** Para usar, você precisa da sua própria cópia legal da ISO.

## Estado

**Boot inicial.** O ELF inteiro recompila com o [PS2Recomp](https://github.com/ran-j/ps2recomp): 78.202 funções, 0 instruções não tratadas. O executável já começa a rodar, passa do travamento da TTY de debug (DECI2) e imprime as primeiras mensagens do jogo. O próximo passo é ligar as funções do SDK da Sony aos handlers HLE do runtime, a começar pela inicialização do SIF/IOP.

Veja [`progression/`](progression/) para o diário de progresso.

## Estrutura

- `docs/`: pesquisa e documentação técnica ([ferramentas](docs/ferramentas.md))
- `progression/`: registro do progresso do projeto, por data
- `runtime/overrides/`: correções específicas do jogo (game overrides do PS2Recomp)
- `tools/`: scripts de export do Ghidra e geração dos bindings do SDK
- `CMakeLists.txt`: build do executável (runtime do PS2Recomp + código gerado + overrides)

## Build (Linux)

Os passos completos estão no [`CLAUDE.md`](CLAUDE.md#comandos). Em resumo:

1. Extraia o disco para `game/` (a ISO não fica no repositório).
2. Exporte a function map do Ghidra (`tools/ghidra_export.sh`) e recompile com o `ps2_recomp`, gerando `generated/`.
3. Rode o `ps2_analyzer` e o `tools/gen_sdk_bindings.py`.
4. `cmake -S . -B build -G Ninja ...` e `cmake --build build --target dra2`.

## Ferramentas

Ghidra 12.1.2 + [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded), [PS2Recomp](https://github.com/ran-j/ps2recomp), PCSX2 (referência). Detalhes em [`docs/ferramentas.md`](docs/ferramentas.md).
