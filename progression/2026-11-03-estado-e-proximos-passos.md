# 2026-11-03: Onde paramos

## Estado
- O ELF recompila inteiro com o PS2Recomp (78.202 funções, 0 instruções não tratadas).
- O executável (build antigo via staging do upstream) roda, passa do bloqueio da TTY/DECI2 e imprime as primeiras mensagens do jogo.
- Para no `sceSifInitRpc` (`0x40AB28`), esperando o IOP. Detalhes em [2026-11-03-ps2recomp.md](2026-11-03-ps2recomp.md).
- **Feito, mas ainda não testado:** 98 bindings HLE do SDK (`runtime/overrides/dra2_sdk_bindings.inc`) e o build próprio (`CMakeLists.txt`). O primeiro build completo em `build/` foi interrompido no início (~60 de 2.562 passos). Retomar com `cmake --build build --target dra2 -j5` (cerca de 40 min na 1ª vez; o ninja continua de onde parou).

## Próximos passos
1. Terminar o build em `build/` e rodar: `cd game && timeout -s INT 60 ../build/dra2 "$PWD/SLUS_210.67"`.
   - Conferir no log: `[dra2] N/98 SDK bindings aplicados` e se o boot passa do `sceSifInitRpc`.
   - Confirmar que um override novo recompila em segundos (o objetivo do build novo).
2. Seguir o loop do upstream: corrigir os bloqueios duros (função não encontrada, syscall TODO, I/O de CD), um de cada vez, pesquisando antes (PCSX2, ps2sdk, issues do PS2Recomp como referência).
3. Levar os nomes do SDK achados pelo analyzer (308, incluindo os 200 sem handler: libc, libm, exceções C++, timers) para o Ghidra, via script headless que aplique os nomes, para que o export e a análise usem esses nomes.
4. Nomear as funções do Lua 5.0 comparando com o código-fonte oficial.
5. Mais adiante: VU1 (8 overlays DVP), GS (avaliar o paraLLEl-GS) e áudio (SPU2).

## Notas
- Os nomes que dei no Ghidra (`tty_write`, `deci2_tty_send`, `deci2_tty_open`, `sceDeci2ReqSend`, `sceDeci2Poll`) estão só no projeto da GUI (`ghidra/DRA2`), não no headless.
- O `external/PS2Recomp/out/build` ainda tem os objetos do build antigo do `ps2EntryRunner` (cerca de 1 GB). Dá para apagar, mas o `ps2_recomp` e o `ps2_analyzer` ficam lá.
