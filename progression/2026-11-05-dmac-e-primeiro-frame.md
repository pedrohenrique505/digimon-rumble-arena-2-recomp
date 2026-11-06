# 2026-11-05: DMAC desligado e o primeiro frame no GS

## Bloqueio resolvido: o DMAC era desligado pelo próprio jogo (patch 0005)
- Com um trace temporário no VIF1, nenhum VIFcode era processado. O contador `dma=` do `run:tick` ficava em 1 o tempo todo.
- O `writeIORegister` do CHCR descarta o kick quando `D_CTRL.DMAE` (bit 0) está zerado, e o `D_CTRL` valia `0x2`.
- Quem escreve: `FUN_00427578` (envio para o scratchpad pelo canal `0x1000D000`) faz `D_CTRL = (D_CTRL & 0xFFFFF8FF) | 2`, uma leitura-modificação-escrita que liga o RELE. O jogo nunca liga o DMAE por conta própria: no console, o kernel deixa esse bit ligado antes de o ELF rodar. No runtime, o registrador começava em 0.
- Correção: `D_CTRL = 1` no reset da memória (`PS2Memory::initialize`). Layout dos bits: [NetBSD dmacreg.h](https://src.rcs.uwaterloo.ca/xref/netbsd-src/sys/arch/playstation2/ee/dmacreg.h) e [ps2tek DMAC](https://israpps.github.io/ps2tek/PS2/DMAC/IO.html). Não achei documentado o valor exato que a BIOS deixa no registrador; a conclusão vem do próprio código do jogo.

## Resultado
- `dma=344` no 1º tick. O VIF1 alterna os buffers `0x57dd00` e `0x5fad80` (com `DIRECT`), e o GIF manda pacotes todo frame.
- **Primeira imagem na tela:** um quadro inteiro verde com marcas pretas repetidas na faixa de baixo. Deve ser uma limpeza de tela ou uma tela inicial ainda incompleta.
- Desempenho: cerca de 2 VBlanks por segundo. O `perf` (`-e cpu-clock:u`, porque `perf_event_paranoid=2`) mostra o tempo quase todo no GS em software (`WritePixel`, `ReadCT32`, `DrawTriangle`, `DrawSprite`, `CopyFrameToHostRgba`). Fora isso, o VU1 aparece com menos de 1%.

## Próximos passos
1. Descobrir o que é o quadro verde: conferir FRAME/TEX0/PSM dos draws (aba GS do debugger, `[gs:reg]`) e checar se é um clear com o formato de cor errado ou só uma tela de transição.
2. Desempenho do GS: avaliar backend por hardware (paraLLEl-GS) ou otimizar o caminho em software.
3. Pendente desde antes: `[IOP] Couldn't bind callback rpc.` (SID `0x20`).

## Item 1 investigado: o quadro verde é artefato do runtime, não do jogo
- Layout da VRAM (dump temporário por página de 8 KB):
  - páginas `0x00` a `0x45`: buffer exibido, FBP 0, CT16 640×448 (`0x8C000` bytes = 70 páginas), todo com `0x8000` (preto, bit A);
  - páginas `0x46` a `0xD1`: alvo de render CT32 (FBP `0x46`), todo com `0x80000000` (preto, A=0x80). O `ZBUF` começa em `0xD2`, logo depois;
  - páginas `0x1DF` a `0x1E8`: dados de textura (índices `0x01`/`0x07`, possivelmente CLUT em `0x1E7`). É o único conteúdo que não vem de limpeza.
- O jogo renderiza no CT32 e copia com um sprite (`TEX0.TBP0 = 0x8C0` = FBP `0x46`) para o CT16 da tela. O FINISH chega a cada frame.
- O verde vem do `PresentFromLocalMemory` do upstream: quando o FBP 0 está todo preto, ele tenta os FRAMEs dos contextos como "candidatos". O FBP 0 lido com PSM CT32 transforma `0x80008000` em RGBA `(0, 0x80, 0, 0x80)`, um verde que ocupa só as 224 primeiras linhas (16 bits lidos como 32).
- **Na prática a tela é preta:** os buffers só recebem limpeza. Os poucos sprites texturizados (`PRIM=6`, `TEX0` em `0x1D00`/T8) não deixam nada visível.

## Próximos passos (revisados)
1. Investigar por que o jogo não sai da tela preta. O principal suspeito é o `[IOP] Couldn't bind callback rpc.` (o IOPHOOEY faz bind no SID `0x20` do EE): sem esse canal, o carregamento por streaming deve estar parado.
2. Considerar um patch que desligue a heurística de "candidato" do upstream (ela engana o diagnóstico), ou deixá-la atrás de uma variável de ambiente.
3. Desempenho do GS em software.

## Callback RPC e o carregamento pelo IOPHOOEY (patches 0006 e 0007)
- **0006, RPC IOP→EE:** no `ps2xIOP`, o `sceSifBindRpc` (sifcmd 15) e o `sceSifCallRpc` (16) eram stubs que retornavam 0, então `client->server` ficava 0 e o IOPHOOEY dizia "Couldn't bind callback rpc.". Agora:
  - `IopHost::bindEeRpc`/`callEeRpc` (métodos virtuais com implementação padrão `false`);
  - o runtime procura o SID em `g_rpc_servers`, copia o envio para o `buf` do servidor do EE e roda a função do servidor via `eeScheduler().queueInvocation` (o papel do `sceSifRpcLoop` do EE); `rsize` bytes do retorno voltam para a RAM do IOP;
  - layout do `SifRpcClientData_t` do IOP no ps2sdk: `command` +16, `buf` +20, `cbuf` +24, `end_function` +28, `end_param` +32, `server` +36. O `end_function` (modo NOWAIT) ainda não é chamado; o jogo não usa.
  - No jogo: o servidor SID `0x20` do EE (`0x3EA170`, thread criada em `0x3E94E8`) só enfileira eventos de 2 words, por exemplo `(0x00FFE390, tamanho)`, depois de abrir ou ler arquivos.
- **0007, `sceSifSetDmaIntr` (sifman 32):** caía no `default` e retornava 0 ("fila de DMA cheia"), então o driver repetia sem parar (cerca de 6.500 vezes por segundo, achado com um histograma temporário dos imports do IOP). Agora faz a mesma cópia do `sceSifSetDma` e agenda o callback de conclusão em `pendingGuestCallbacks`. Ordinais: [ps2sdk sifman exports.tab](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/sifman/src/exports.tab).
- Laço de carga do EE: `FUN_0037FB48` faz `while (pendentes && status_io != 5) frame();`, com o status vindo do `fno 5` do servidor "io" (SID `0x12`, client `0x564D98`).
- Resultado: o carregamento anda (abre → evento com tamanho → lê → fecha → próximo arquivo).

## Novo bloqueio: funções que o Ghidra não reconheceu
- `JALR` indireto para `0x3DE1F8`: um buraco no function map (`0x3DE1F4..0x3DE218`) com 4 folhas de C++ (`jr ra` com `nop` ou `li v0,1`). Resolvido provisoriamente por override (`leafReturnVoid`/`leafReturnOne`).
- Logo depois vem `0x29FF38`, do mesmo tipo. Um script achou **1.200 buracos com `jr ra`** entre as funções do `.text` no CSV exportado. Correção certa: criar funções nesses buracos no Ghidra headless, reexportar, regenerar e reconfigurar o build.

## Buracos no function map resolvidos no export (CreateGapFunctions)
- `tools/ghidra/CreateGapFunctions.java` roda no headless antes do `ExportPS2Functions.java`. Os buracos são calculados como `.text*` menos a união dos corpos das funções. A 1ª versão pulava para o `getMaxAddress()` do corpo, e corpos não contíguos faziam o laço saltar trechos grandes, por isso achava 0.
- Resultado: 9 buracos com `jr ra`, 31 funções novas (ex.: `0x1F7CCC..0x1F7D04`, `0x2802BC..0x280394`, `0x36F4C0..`). O CSV final não tem nenhum buraco com `jr ra` em `.text`. As folhas `0x3DE1F8` e `0x29FF38` já estavam no mapa de um export anterior, e os overrides `leafReturnVoid`/`leafReturnOne` foram removidos.
- Mesma estratégia do [BT3-Recomp](https://github.com/z3xox/BT3-Recomp) (`games/bt3/dbzp_gaps.csv`, `dbzp_missing.csv`). Ele também tem um interpretador MIPS de fallback (`ps2xRuntime/src/lib/ps2_interp.cpp`, `PS2Runtime::interpretUntil`) para destinos não recompilados, que é candidato a patch nosso se aparecerem mais buracos.
- Execução de 60 s: nenhum `missing-target`. Antes, o jogo parava em `pc=0x29FF38` com `dma=128`; agora o PC anda e o DMA sobe (`dma=1026` no tick 480).

## Referências de outros ports
- BT3-Recomp (PS2Recomp): função por buraco, interpretador de fallback, máscara de ponteiros kseg0 (`& 0x1FFFFFFF`), DMAE religado no reset da libdma, paraLLEl-GS opcional, `PS2X_ASYNC_KICK` (VIF/VU1/GIF numa thread), `gen_vu1.py` (VU1 → C++).
- [Downhill Revived](https://github.com/skuza-oss/downhill-revived): decomp matching, não recomp. Serve de referência para o renderer nativo (`port/gs`, `port/render`).

## Próximos passos
1. Ver o que aparece na tela agora (screenshot) e acompanhar a carga com `DRA2_TRACE_RPC=1`.
2. Desempenho do GS em software (cerca de 8 VBlanks/s agora).
