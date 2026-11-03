# Digimon Rumble Arena 2 — Static Recompilation

Recompilação estática de **Digimon Rumble Arena 2** (PS2, NTSC-U, `SLUS-21067`) para um executável nativo de PC. Por enquanto o alvo é só Linux.

O código MIPS R5900 do Emotion Engine é traduzido para C++ e ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). Não é um emulador: o jogo vira um programa nativo.

> **Este repositório não contém nenhum arquivo do jogo.** Para usar, você precisa da sua própria cópia legal da ISO.

## Estado

Início do projeto: pesquisa e análise do binário. Veja [`progression/`](progression/) para o diário de progresso.

## Estrutura

- `docs/`: pesquisa e documentação técnica ([ferramentas](docs/ferramentas.md))
- `progression/`: registro do progresso do projeto, por data

## Ferramentas

Ghidra 12.1.2 + [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded), [PS2Recomp](https://github.com/ran-j/ps2recomp), PCSX2 (referência). Detalhes em [`docs/ferramentas.md`](docs/ferramentas.md).
