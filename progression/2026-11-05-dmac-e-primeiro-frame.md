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
