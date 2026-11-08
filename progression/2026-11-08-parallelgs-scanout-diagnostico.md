# Scanout do paraLLEl-GS: exportação e estado de vídeo ausente

Continuação do shadow, sem promoção a backend autoritativo.

## Análise e evidência

- Ghidra GUI confirmou `SLUS_210.67`, SHA-256
  `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
  A decompilação de `0x003a2460` reconfirmou escritas de PMODE, SMODE2,
  DISPFB/DISPLAY e BGCOLOR, envio de buffers e incremento de `$gp+0x440`.
  Isso é evidência estática, não observação de execução.
- REA `inspect_with_objdump(file_headers)` confirmou ELF32 little endian,
  entry `0x100008`, digest inalterado antes/depois. Evidence pai
  `ev_5b0057281986d06a5eb8b00fdccaf348cf31f05ebb819f58f979af3a668c83d9`,
  projeção `ev_3cde656108b7ce50b2f2480fcd1deb552bd94eb23791779c605a26328541a07b`.
  O objdump local reporta arquitetura UNKNOWN; R5900 é identificação do Ghidra.
- Replay com exportação sem SMODE1 reproduziu `Unknown video format` e
  `last latch has no scanout image` na captura longa. O formato registra sete
  registradores de exibição e omite SMODE1. O HLE GsSetCrt também não o escreve;
  o shadow não copia SMODE1. Não basta copiar o zero atual.
- Fonte primária consultada: [replayer upstream na revisão pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/tools/gs_repro_replayer.cpp),
  e [seleção do modo no renderizador](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_renderer.cpp).
  O candidato exige CMOD/LC reconhecidos para montar o scanout.

## Entrega e validação

Commit `03d4f38`: `--scanout-ntsc` exporta o último resultado de vsync como
PPM RGB e JSON com dimensões e hipótese explícita. Readback offline síncrono
separado em `scanout_export_seconds`; não é apresentação ao vivo.
Sem hipótese, o replay anterior mantém seu comportamento. Sem imagem na
exportação, falha; arquivos existentes não são sobrescritos.

- Build CPU/Vulkan passou; warning herdado de `std::is_pod` no candidato.
- `tools/test_gs_capture.py`: 15/15 em cada backend após a alteração.
- `tools/test_gs_scanout.py`: GPU verifica todos os pixels RGB de um scanout
  uniforme 640×224 (NTSC sem interlace), metadados, recusa de sobrescrita e
  captura sem latch. CPU recusa a opção GPU antes de escrever resultados.
- Captura longa `capture-menu/complete.gs`: 170.183 registros, 660 latches,
  22 referências; exportou scanout 640×448 no latch 660 com NTSC explícito.
  As 22 referências de VRAM continuam divergindo, sem aceite visual.
- Artefatos em `.cache/performance/parallelgs-scanout/`, ignorados pelo git.
  Nenhum dado do jogo foi versionado. Não houve nova sessão nativa REA aberta.

## Implicação e próxima etapa

Os 37,25× históricos são resultado de replay GS, mas não comprovam custo de
scanout com imagem válida: chamar vsync e descartar seu resultado não verifica
isso. Esta execução é diagnóstica e não substitui os cinco pares do benchmark.
A equivalência visual continua aberta. Antes de conectar a apresentação à
janela, implementar o estado de vídeo no HLE GsSetCrt, preservar SMODE1 na
captura e no shadow, e validar o scanout sem hipótese NTSC. Depois medir
novamente o replay e completar integração de dispositivo/threads da janela.
