# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Projeto

Recompilação estática de **Digimon Rumble Arena 2** (PS2 NTSC-U, `SLUS-21067`, VER 1.02) para um executável nativo. O alvo por enquanto é só Linux. O código MIPS R5900 do Emotion Engine vira C++, que é ligado a um runtime que substitui o hardware do PS2 (GS, VU1, IOP, SPU2). A documentação e os commits são em português.

Estado atual: o jogo roda o laço principal (IOP emulado com os IRX do jogo, leitura do disco, DMA de render por frame). O bloqueio atual é o pipeline VIF1/VU1 → GS. Veja o `progression/` mais recente. Base: PS2Recomp (ran-j).

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
- `generated/`: saída do `ps2_recomp` (cerca de 78k `.cpp`, 552 MB). Depois de regenerar, **reconfigure** o `build/`, porque o glob roda só na configuração.
- `build/`: build do jogo, com o executável `build/dra2`.
- `ghidra/analyzer/`: saída do `ps2_analyzer`.

## Comandos

```bash
# Importa e analisa no Ghidra headless (só na 1ª vez) e exporta TOML + CSV para o PS2Recomp
tools/ghidra_export.sh

# Para reanalisar do zero, apague ghidra/headless/ antes

# Ferramentas do PS2Recomp (ps2_recomp, ps2_analyzer). Ficam em external/PS2Recomp, fora do git.
# GCC/Linux precisa de -march=x86-64-v3 (issue upstream #261). RelWithDebInfo sem -g: o tipo Release liga o LTO.
cmake -S external/PS2Recomp -B external/PS2Recomp/out/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
cmake --build external/PS2Recomp/out/build --target ps2_recomp ps2_analyzer

# Recompilar: ghidra/export/recomp.toml = TOML exportado com output -> generated/
external/PS2Recomp/out/build/ps2xRecomp/ps2_recomp ghidra/export/recomp.toml

# Funções do SDK Sony (banco de assinaturas embutido no analyzer) -> bindings HLE no override
external/PS2Recomp/out/build/ps2xAnalyzer/ps2_analyzer game/SLUS_210.67 ghidra/analyzer/analyzer.toml
python3 tools/gen_sdk_bindings.py

# Patches nossos no clone do upstream (idempotente; rodar depois de clonar ou atualizar o PS2Recomp)
tools/apply_upstream_patches.sh

# Build do jogo (CMakeLists.txt da raiz): runtime do upstream como biblioteca + generated/ + overrides
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
cmake --build build --target dra2 -j5   # -j5 cabe nos 7 GB de RAM

# Rodar (o cdRoot é o diretório do ELF, então game/ precisa do disco inteiro extraído).
# DRA2_ISO = ISO original: o IOP lê os setores reais (sem ela, usa um ISO virtual montado a partir de game/).
cd game && DRA2_ISO="$HOME/Downloads/Digimon Rumble Arena 2 (USA)/Digimon Rumble Arena 2 (USA).iso" \
  timeout -s INT 60 ../build/dra2 "$PWD/SLUS_210.67"
# Diagnóstico: DRA2_TRACE_RPC=1 (cada sceSifCallRpc), DRA2_TRACE_GS=1 (CSR/IMR/flag do limitador, AddIntcHandler/EnableIntc)
```

Detalhes do export: o `ExportPS2Functions.java` (de `ran-j/PS2Recomp`) fica em `~/ghidra_scripts/`. No modo headless, cada `askFile()` consome um argumento do script, em ordem (TOML, depois CSV). O `.properties` não funcionou, porque o Ghidra prioriza a cópia em `~/ghidra_scripts`.

## Ambiente

- Ghidra 12.1.2 (pacote `extra` do Arch, em `/opt/ghidra`) com as extensões em `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/`: `ghidra-emotionengine-reloaded` v2.1.39 (precisa ser a build da mesma versão do Ghidra) e `GhidraMCP`.
- O GhidraMCP dá acesso ao programa aberto na GUI (decompilar, renomear, comentar, xrefs, strings). Ele não executa scripts; para isso use o headless.
- Ao atualizar o Ghidra, troque o zip do plugin EE pela build correspondente.

## Arquitetura do build

- `CMakeLists.txt` (raiz) inclui `ps2xIOP` e `ps2xRuntime` do upstream com `EXCLUDE_FROM_ALL` e define:
  - `dra2_generated`: biblioteca estática com o código gerado (unity build de 32, PCH, `-O2`);
  - `dra2`: executável com o `main.cpp` do upstream, o `generated/register_functions.cpp` (**`-O0`**: é um construtor com cerca de 360k atribuições, que leva 28 min em `-O2`) e `runtime/overrides/*.cpp`.
- **Correções por jogo** ficam em `runtime/overrides/` (código nosso, versionado), usando a API `game_overrides.h`: só se aplicam se ELF, entry e CRC32 (`0xA6702034`) baterem. Os bindings de endereço para handler são feitos em runtime (`replaceFunction`), sem recompilar o código gerado. As chamadas `jal` geradas passam por `runtime->dispatchGuestBranch`, então a substituição pega.
- **Convenção dos handlers:** `ctx->pc = $ra` **antes** de chamar qualquer HLE do runtime, como fazem os stubs gerados. HLEs que guardam o contexto para retomar depois (ex.: `sceSifCallRpc` com callback) entram em laço se o `pc` ainda apontar para a função. Por isso os bindings do SDK passam pelo `sdkTrampoline`, e não pelo `bindAddressHandler` do upstream.
- Correção por jogo vai em override. Correção **genérica** do runtime/IOP que não cabe num override vira **patch** em `patches/ps2recomp/` (formato `git format-patch`, um por correção, aplicado pelo `tools/apply_upstream_patches.sh`). No clone há um branch local `dra2-patches` com um commit por patch: edite lá e regenere com `git format-patch -o patches/ps2recomp e563698..dra2-patches`. Cada patch é candidato a PR no upstream.

## Fatos do binário (para não redescobrir)

- ELF stripped (sem `.symtab`/`.mdebug`), C++ com exceções, entry `0x100008`, `$gp = 0x5384F0`, linguagem Ghidra `r5900:LE:32:default`.
- 9.760 funções após a auto-análise. As stubs de syscall do kernel estão em `0x004081A0` a `0x00408A10` e já saem nomeadas no export.
- Bibliotecas linkadas estaticamente: **Lua 5.0** (scripts `lua/game`, `lua/shellinit`) e o SDK Sony (libpad, libmtap, libmc, sceCd, SIF RPC, libgraph), além da libc. Elas devem virar stubs/HLE no runtime, não ser recompiladas. Nomeá-las no Ghidra é pré-requisito para o export classificá-las.
- Seções `.text.*` nomeadas (`collision`, `aiNodegraph`, `renderMeshInstances`, `syncSkeleton`…) e microcódigo VU1 em `.vutext` e em 8 overlays `.DVP.overlay*`.
- O plugin cria espelhos da RAM em `0x20000000` e `0x30100000`, por isso as strings aparecem 3x. Use só `0x001xxxxx` a `0x005xxxxx`.
- IOP: `IOPRP280.IMG` (SDK 2.8) e um módulo custom, `IOPHOOEY.IRX`.

## Diário de progresso

`progression/AAAA-MM-DD-titulo.md`: uma entrada por sessão ou marco, com o que foi feito, as descobertas e os próximos passos. Atualize ao fim de cada etapa relevante.
