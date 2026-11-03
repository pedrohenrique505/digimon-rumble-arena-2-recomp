# Pesquisa de ferramentas: recompilação estática de Digimon Rumble Arena 2 (PS2, SLUS-21067)

Pesquisa feita em 2026-11-03.

## O que é o alvo

Inspecionado direto da ISO:

- Plataforma: PS2 (NTSC-U). `SYSTEM.CNF` faz boot de `SLUS_210.67`, VER 1.02. CRC no PCSX2: `568A5C78`.
- ELF: MIPS R5900, `eabi64`, entry em `0x100008`. **Não tem `.symtab` nem `.mdebug`**, então não há símbolos e as funções vão ter que ser identificadas no Ghidra.
- `.text` principal tem cerca de 3,1 MB, mais várias seções `.text.<nome>` com nomes que já dão pistas (`collision`, `aiNodegraph`, `drawing`, `render...`, `sortAn...`, `syncSk...`).
- Tem `.gcc_except_table`, ou seja, o jogo é C++ com exceções.
- Tem `.vutext`/`.vudata` e 8 overlays `.DVP.overlay*` com `.DVP.ovlytab`. São microprogramas de VU1, provavelmente o pipeline de render, que vão precisar ser recompilados ou interpretados.
- `LUA.RES` sugere que a lógica de jogo roda em script Lua embutido.
- Módulos IOP: `IOPRP280.IMG` (SDK 2.8), `CDVDSTM`, `LIBSD`, `SDRDRV`, `SPUCODEC`, `PADMAN`, `MTAPMAN`, `MCMAN/MCSERV`, `SIO2MAN` e um custom, `IOPHOOEY.IRX`.
- Não achei nenhum projeto de decomp ou RE desse jogo. Existe um protótipo de 17/06/2004 no Hidden Palace, que pode ser útil para comparação caso tenha símbolos.

## Recompiladores estáticos de PS2

| Ferramenta | O que faz | Status | Observações |
|---|---|---|---|
| [ran-j/PS2Recomp](https://github.com/ran-j/ps2recomp) | ELF para C++, com runtime (`ps2xAnalyzer`, `ps2xRecomp`, `ps2xRuntime`, `ps2xIOP`) | Experimental, nenhum jogo jogável listado | GPL-3.0. Fluxo para ELF stripped: Ghidra, depois `ExportPS2Functions.java`, depois TOML. Tem stubs, `handler@0xADDR` e patches. Testado mais com MSVC. GS/VU fracos. |
| [luisbraganca/ps2n](https://github.com/luisbraganca/ps2n) | ELF para C++, com runtime (kernel EE, GS software, IOP, SPU2 via SDL) | Cobre 359/359 instruções EE. 007: Agent Under Fire roda menus e a 1ª missão | Tem modo `--interpret` para isolar bugs do recompilador. Desenvolvido no Windows, Linux não testado. Licença não declarada. |
| [Harold0777/eagle_five_recomp](https://github.com/Harold0777/eagle_five_recomp) | Recomp específico de Ace Combat 5 | v0.3.0, roda no Linux com Vulkan | **Melhor referência prática.** Faz lift dos 13 microprogramas de VU1 para C, tem GS em Vulkan e interpretador de fallback. Apache-2.0. |
| [snuri00/ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) | Recomp para WebAssembly/WebGPU | Ativo | Útil como referência de FPU/MMI/delay slots. |
| [BlackLineInteractive/SHO-GTA-VCS-PS2Recomp](https://github.com/BlackLineInteractive/SHO-GTA-VCS-PS2Recomp) | Fork do PS2Recomp para Silent Hill Origins e GTA VCS | n/d | Exemplo de configuração por jogo. |

## Análise e engenharia reversa

- **Ghidra** (já instalado), com o plugin [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded). Decompila instruções EE (MMI, COP2), importa savestates do PCSX2 e suporta Ghidra 12.1. É essencial aqui porque o ELF é stripped. Também há um MCP do Ghidra disponível nesta sessão.
- **[splat](https://github.com/ethteck/splat)**: divide o binário em asm/data por segmento. Útil se a gente quiser seguir para uma decomp parcial depois.
- **PCSX2**: debugger (breakpoints, stepping, memória) e trace log. Serve de referência para comparar o comportamento do recomp com o emulador.
- **[vutrace](https://github.com/chaoticgd/vutrace)**: trace de VU1 via PCSX2 com patch, para validar os microprogramas recompilados.
- **[ps2_ida_vu_micro](https://github.com/Goatman13/ps2_ida_vu_micro)** e **ps2-dvp-binutils**: disassembler e assembler de microcódigo VU.

## Gráficos, áudio e plataforma

- **[paraLLEl-GS](https://github.com/Themaister/parallel-gs)**: GS em compute shaders Vulkan, standalone (não usa GSdx). Tem o `parallel-gs-stream` para testar dumps de GS isoladamente. É o melhor candidato a backend de GS. A API de biblioteca ainda não é estável.
- O GS do PCSX2 (GSdx) é a alternativa mais madura, mas está muito acoplado ao emulador.
- SDL3 para janela, input e áudio. O SPU2 precisa ser emulado ou receber HLE de `LIBSD`/`SDRDRV`/`SPUCODEC`.

## Recomendação inicial

1. Base: **PS2Recomp** (mais ativo, com mais forks e workflow via Ghidra) **ou ps2n** (runtime mais completo hoje). Vale testar os dois no ELF antes de decidir.
2. Ghidra com o plugin EE para mapear funções e exportar a function map.
3. Usar o **eagle_five_recomp** como referência para VU1 lifting e GS em Vulkan no Linux.
4. Validar contra o PCSX2.

## Fontes

- https://github.com/ran-j/ps2recomp
- https://github.com/luisbraganca/ps2n
- https://github.com/Harold0777/eagle_five_recomp
- https://github.com/snuri00/ps2-web-recomp
- https://github.com/chaoticgd/ghidra-emotionengine-reloaded
- https://github.com/ethteck/splat
- https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation
- https://github.com/chaoticgd/vutrace
- https://github.com/Goatman13/ps2_ida_vu_micro
- https://hiddenpalace.org/Digimon_Rumble_Arena_2_(Jun_17,_2004_prototype)
- https://tcrf.net/Proto:Digimon_Rumble_Arena_2
- https://www.timeextension.com/news/2026/01/native-pc-ports-of-ps2-games-could-be-on-the-way-thanks-to-new-recompilation-experiment
