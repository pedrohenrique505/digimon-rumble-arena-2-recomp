# paraLLEl-GS: shadow executado no jogo (etapa 3)

Continuação da etapa 3 a partir do adaptador shadow (`9377b72`, `a372643`).
O backend CPU continua autoritativo para VRAM, sinais e apresentação;
a GPU espelha o fluxo. Nada aqui promove a GPU a backend autoritativo.

## Identidade (Ghidra + REA)

Ghidra (GUI do usuário) e REA 6.8.0 (`inspect-native-exports`)
reconfirmaram o mesmo artefato: `SLUS_210.67`, MIPS R5900 LE, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`,
Evidence ID `ev_5123ea7e012701c3bf310d62ee24731816d4785ff6a07accd0793deca81b021c`.
É a mesma evidência das sessões anteriores; nenhuma alegação nova de
execução decorre dela.

## Investigação

- Único chamador de `sceGsExecStoreImage` no Ghidra: `FUN_003a2948`
  (xref de `0x003a2ad8`), que lê blocos de `0x20` linhas em
  `param_2 + 0x14` (ícone/save). É o caminho local→host do jogo; no
  shadow ele só consome do FIFO da GPU para manter sincronia, sem
  servir o EE.
- Na captura longa (170.183 registros, abertura+menu): `TRXDIR` só
  host→local (203×); local→host e local→local ausentes. `FINISH` 630×
  (~1/latch); `SIGNAL`, `LABEL` e `TEXFLUSH` ausentes; `TEX0` 4.150×
  (CLUT resolvida dentro do candidato). Cobertura exercitada pela
  captura não é a cobertura do jogo inteiro.
- `GS::latchHostPresentationFrame` chama `onLatch` sem `m_stateMutex`:
  o `vsync` do sink podia correr em paralelo com `gif_transfer` da
  thread EE. O replay offline é single-thread e não expõe isso.

## Implementação

- `cda987d`: mutex próprio nos 8 callbacks do sink; cauda sub-qword do
  FIFO local→host reconstruída pelo oráculo CPU sobre a VRAM da GPU
  sincronizada (mesma estratégia do replay; antes eram zeros);
  estatísticas `[dra2-gs] shadow` a cada 300 latches no stderr.
- Build com `DRA2_ENABLE_PARALLEL_GS=ON` na revisão pinada
  `cc6184af7e0c03da603045ca371ffa5dae9b0655`. O relink pagou o backlog
  de ~2 mil ações do unity gerado já registrado no NEXTSTEPS; não é
  custo do ParallelGS. `BUILD_EXIT=0`; só warning herdado de
  `std::is_pod` no cabeçalho do candidato.

## Validação

- `gpu-shadow` por 60 s e 120 s a partir de `game/`, com a ISO
  original: sink instalado e ativo, Vulkan na Radeon Renoir/RADV,
  sem crash (processos encerrados pelo `timeout`). Estatísticas:
  latch 300 com 7.327 e 88.248 pacotes GIF nas duas rodadas (a cena
  muda o GIF/latch); `nativeFallbacks=0`, `fifoReq=0` nessas cenas.
- Progressão equivalente ao CPU: as três rodadas (shadow 60 s,
  shadow 120 s, CPU 60 s) mostram os mesmos 128 uploads de frame
  registrados, com ticks ~130–168. O shadow não travou nem atrasou
  a abertura de forma visível nesses logs. Isso não é medição de FPS.
- Regressão do replay: 15/15 casos em CPU e 15/15 em Vulkan
  (`tools/test_gs_capture.py`).
- Sem `DRA2_GS_BACKEND`, nenhum log `[dra2-gs]`: caminho CPU intacto.

## Pendências

- Apresentação do framebuffer da GPU na janela, sinais SIGNAL/FINISH
  a partir da GPU e promoção a backend autoritativo continuam abertos.
- A divergência visual da captura longa (etapa 2) segue sem aceite;
  o speedup de 37,25× continua sendo só do replay offline.
- Nenhum dado do jogo foi adicionado ao git.
