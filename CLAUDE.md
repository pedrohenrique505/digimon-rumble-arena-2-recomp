# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Projeto

Recompilação estática de **Digimon Rumble Arena 2** (PS2 NTSC-U, `SLUS-21067`, VER 1.02) para um executável nativo. O alvo por enquanto é só Linux. O código MIPS R5900 do Emotion Engine vira C++, que é ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). A documentação e os commits são em português.

Estado atual: análise e preparação. Base atual: PS2Recomp (ran-j), já recompilando o ELF inteiro sem instruções não tratadas. O ps2n continua como alternativa; veja `docs/ferramentas.md`.

## Regra: pesquisar na internet ao encontrar problemas

**Sempre que surgir um problema, dúvida de hardware ou comportamento inesperado, faça uma busca na internet antes de tentar resolver por conta própria.** O PS2 é um hardware muito documentado. Quase sempre alguém já resolveu o mesmo problema:
- Código e issues do PCSX2 (comportamento exato de GS, VIF, DMA, VU, IOP, SPU2)
- Docs do ps2sdk e do PS2Tek, specs do EE e do GS
- Issues e forks do PS2Recomp, ps2n, eagle_five_recomp (Ace Combat 5) e outros recomps e decomps de PS2
- Issues do plugin ghidra-emotionengine-reloaded e do splat

Ao encontrar algo útil, cite a fonte no `progression/` ou em `docs/`.

## Regra crítica: nenhum arquivo do jogo no git

ISO, ELF, IRX, `.RES`, conteúdo extraído, savestates, projetos Ghidra e **código gerado pelo recompilador** (derivado do código do jogo) nunca são versionados. O `.gitignore` cobre isso. Ao adicionar uma pasta ou extensão nova, confira com `git check-ignore -v <path>` e revise `git status` antes de cada commit.

## Layout local (fora do git)

- `game/`: `SLUS_210.67`, `SYSTEM.CNF` e `IOP/*.IRX`, extraídos da ISO em `~/Downloads/Digimon Rumble Arena 2 (USA)/`.
- `ghidra/DRA2`: projeto da GUI do usuário. Fica travado enquanto a GUI está aberta, então **não use em headless**.
- `ghidra/headless/DRA2_headless`: projeto separado, usado pelo script de export.
- `ghidra/export/`: `SLUS_210.67.toml` (config do PS2Recomp) e `SLUS_210.67.csv` (function map).
- `external/PS2Recomp/`: clone do upstream (GPL-3.0) com build em `out/build`.
- `generated/`: saída do `ps2_recomp` (cerca de 78k `.cpp`, 552 MB), ligada por symlink em `external/PS2Recomp/ps2xRuntime/src/runner/`.

## Comandos

```bash
# Importa e analisa no Ghidra headless (só na 1ª vez) e exporta TOML + CSV para o PS2Recomp
tools/ghidra_export.sh

# Para reanalisar do zero, apague ghidra/headless/ antes

# PS2Recomp (clonado em external/PS2Recomp, fora do git). GCC/Linux precisa de -march=x86-64-v3 (issue upstream #261)
# Build de desenvolvimento: RelWithDebInfo sem -g. O upstream liga o LTO só no tipo Release
# (ReleaseMode.cmake), e o link com LTO de 78k funções leva de 20 a 40 min e estoura a RAM. mold linka em segundos.
cmake -S external/PS2Recomp -B external/PS2Recomp/out/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
cmake --build external/PS2Recomp/out/build --target ps2_recomp

# Recompilar: ghidra/export/recomp.toml = TOML exportado com output -> generated/
external/PS2Recomp/out/build/ps2xRecomp/ps2_recomp ghidra/export/recomp.toml

# Ligar generated/ no runtime (symlinks) e compilar o executável. -j5 cabe nos 7 GB de RAM (cerca de 350 MB por job)
tools/stage_runner.sh
cmake --build external/PS2Recomp/out/build --target ps2EntryRunner -j5
```

Detalhes do export: o `ExportPS2Functions.java` (de `ran-j/PS2Recomp`) fica em `~/ghidra_scripts/`. No modo headless, cada `askFile()` consome um argumento do script, em ordem (TOML, depois CSV). O `.properties` não funcionou, porque o Ghidra prioriza a cópia em `~/ghidra_scripts`.

## Ambiente

- Ghidra 12.1.2 (pacote `extra` do Arch, em `/opt/ghidra`) com as extensões em `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/`: `ghidra-emotionengine-reloaded` v2.1.39 (precisa ser a build da mesma versão do Ghidra) e `GhidraMCP`.
- O GhidraMCP dá acesso ao programa aberto na GUI (decompilar, renomear, comentar, xrefs, strings). Ele não executa scripts; para isso use o headless.
- Ao atualizar o Ghidra, troque o zip do plugin EE pela build correspondente.

## Fatos do binário (para não redescobrir)

- ELF stripped (sem `.symtab`/`.mdebug`), C++ com exceções, entry `0x100008`, `$gp = 0x5384F0`, linguagem Ghidra `r5900:LE:32:default`.
- 9.760 funções após a auto-análise. As stubs de syscall do kernel estão em `0x004081A0` a `0x00408A10` e já saem nomeadas no export.
- Bibliotecas linkadas estaticamente: **Lua 5.0** (scripts `lua/game`, `lua/shellinit`) e o SDK Sony (libpad, libmtap, libmc, sceCd, SIF RPC, libgraph), além da libc. Elas devem virar stubs/HLE no runtime, não ser recompiladas. Nomeá-las no Ghidra é pré-requisito para o export classificá-las.
- Seções `.text.*` nomeadas (`collision`, `aiNodegraph`, `renderMeshInstances`, `syncSkeleton`…) e microcódigo VU1 em `.vutext` e em 8 overlays `.DVP.overlay*`.
- O plugin cria espelhos da RAM em `0x20000000` e `0x30100000`, por isso as strings aparecem 3x. Use só `0x001xxxxx` a `0x005xxxxx`.
- IOP: `IOPRP280.IMG` (SDK 2.8) e um módulo custom, `IOPHOOEY.IRX`.

## Diário de progresso

`progression/AAAA-MM-DD-titulo.md`: uma entrada por sessão ou marco, com o que foi feito, as descobertas e os próximos passos. Atualize ao fim de cada etapa relevante.
