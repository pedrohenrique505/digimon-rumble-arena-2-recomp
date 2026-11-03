# 2026-11-03: Primeira análise no Ghidra

## Setup
- Projeto Ghidra em `ghidra/DRA2` (fora do git); ELF em `game/SLUS_210.67` (fora do git).
- Linguagem `r5900:LE:32:default` (plugin EE v2.1.39). `$gp = 0x5384F0`.
- Análise padrão, sem Aggressive Instruction Finder e sem Decompiler Parameter ID.
- MD5 `56aab8af3963464ebf09d6d2acfac13c`, SHA1 `72e00ee55b6b032d6d8a7279401e7dc8e56e2f5d`.

## Resultado
- **9.760 funções** encontradas, todas sem nome (`FUN_xxxxxxxx`). Entry em `0x100008`.
- O plugin cria espelhos da RAM em `0x20000000` (uncached) e `0x30100000` (UCAB), por isso as strings aparecem 3x. Considerar só `0x001xxxxx` a `0x005xxxxx`.
- Não há RTTI legível nem nomes de arquivo `.cpp` nas strings.

## Bibliotecas identificáveis (linkadas estaticamente)
- **Lua 5.0** (`$Lua: Lua 5.0 Copyright (C) 1994-2003 Tecgraf...`), com scripts `lua/game` e `lua/shellinit`. Dá para nomear as funções da VM comparando com o código-fonte do Lua 5.0, que é aberto.
- **Sony SDK (libs EE)**: `sceSifBindRpc`, `sceCdInit`, `scePadPortOpen`/`scePadGetState` (libpad), `sceMtap*` (libmtap, multitap), `sceMcInit` (libmc), `sceGsExecStoreImage` (libgraph). São candidatas a stubs/HLE no runtime.

## Pistas do engine
- Menus por "carousel": `menu.main`, `menu.arena`, `menu.options`, `menu.pause`, `menu.language`, `menu.digidex`.
- Recursos versionados (`Mismatched resource file version in "%s"`). Os `.RES` têm um formato próprio com header de versão.
- Flags de linha de comando de debug: `-dumpversions`, `-version`.
- Cutscenes em `movies/scenes/`.

## Próximos passos
- [ ] Rodar `ExportPS2Functions.java` e gerar o TOML/CSV para o PS2Recomp.
- [ ] Clonar e compilar PS2Recomp (e ps2n) no Linux.
- [ ] Nomear as funções do Lua 5.0 e do SDK Sony a partir das strings.
