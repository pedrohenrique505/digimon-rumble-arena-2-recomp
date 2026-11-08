# CRT no HLE e captura GS V3 com scanout observado

Continuação da investigação de SMODE1. O CPU continua autoritativo para VRAM,
interrupções, readback e janela. Não houve promoção da GPU nem benchmark novo.

## Causa e caminho efetivamente usado pelo jogo

O comentário antigo de `GsSetCrt` descrevia os IDs errados. NTSC é `0x02` e
PAL é `0x03`, como mostram as fontes primárias do ps2sdk:

- [graph_mode.c: modos e configuração SMODE1](https://github.com/ps2dev/ps2sdk/blob/master/ee/graph/src/graph_mode.c)
- [gs_privileged.h: campos SMODE1](https://github.com/ps2dev/ps2sdk/blob/master/common/include/gs_privileged.h)
- [libgs.c: GsResetGraph delega a SetGsCrt](https://github.com/ps2dev/ps2sdk/blob/master/ee/libgs/src/libgs.c)

O Ghidra confirmou o mesmo ELF `SLUS_210.67`, linguagem R5900 LE e SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
Os únicos dois xrefs para `SetGsCrt` em `0x004081a0` vêm de `0x003f0218` e
`0x003f0290`, ambos em `0x003f0120` (`sceGsResetGraph` no binding SDK).
A decompilação mostra a delegação nos modos 0 e 5. São observações estáticas.

O teste direto do syscall falhou antes da correção com
`NTSC HLE did not select analog NTSC in SMODE1`. Após o patch 0017, passou.
Uma primeira captura de 35 s ainda retornava scanout vazio: o binding SDK
substitui `sceGsResetGraph`, então o jogo não executa as chamadas MIPS internas
nem chega ao syscall. O teste foi ampliado para esse HLE e falhou com
`sceGsResetGraph bypassed CRT HLE`. O patch 0019 restaurou a delegação e o
mesmo teste passou, incluindo modo 5 e preservação dos argumentos do chamador.

REA inspecionou o símbolo nativo `ps2_stubs::sceGsResetGraph` no executável de
teste: a instrução em `0x141b68` chama `ps2_syscalls::SetGsCrt`. Evidence pai
`ev_5c098b9aae28a9fac3fc0078ae40a4074fdd4be92e998bf1b3965674453f9a56`,
projeção `ev_4c2ed681378857587f87333622478915ca30b733501110580d6f6115cbddd4b2`.
SHA-256 do executável observado:
`26ec2a6da08259931a5766cf80d5e29f29fd7c37903f733fafebfdf7d796d61a`.
A análise de assembly não é evidência de execução; os testes e logs do jogo
fornecem essa parte. Não foi aberta nova sessão nativa REA.

## Implementação em commits pequenos

- `644a69a`: patch 0017, SMODE1 NTSC/PAL no HLE e alvo `dra2-test-gs-crt`.
  Preserva GCONT, libera PRST/SINT e evita reter um modo TV para CRT ID não
  implementado. Não fornece uma tabela BIOS completa de modos VGA/HDTV.
- `b71050b`: patch 0019, delegação de `sceGsResetGraph` nos modos 0 e 5,
  com contexto privado para rearranjar argumentos sem alterar o chamador.
- `2fbca1d`: idempotência do aplicador quando patches posteriores alteram
  arquivos criados por patches anteriores. Reconhece a identidade do commit
  local, além do reverse-check. O patch 0015 foi convertido do diff bruto para
  format-patch do commit já existente, preservando o conteúdo aplicado.
- `1c9d27f`: patch 0018 e captura `DRA2GS03`; SMODE1 é o oitavo u64 do latch.
  Reader/finalizador continuam aceitando V2, validando o tamanho por versão.
  Shadow copia SMODE1, registra a primeira imagem e conta scanouts vazios.
  `--scanout` usa estado capturado; `--scanout-ntsc` permanece diagnóstico legado.

## Validação

- Build `dra2` e `dra2-test-gs-crt`: passou. Build replayers CPU/Vulkan:
  passou, com warning herdado de `std::is_pod` no candidato.
- Teste HLE: syscall NTSC/PAL, transições, GCONT, PRST/SINT, invalidação de
  modo não implementado, ResetGraph modos 0/1/5 e preservação dos argumentos.
- `tools/test_gs_capture.py`: 16/16 em CPU e 16/16 em Vulkan, incluindo header V3.
- `tools/test_gs_scanout.py`: todos os pixels RGB de scanouts sintéticos NTSC
  e PAL com SMODE1 capturado; diagnóstico NTSC legado, metadados, ausência de
  latch, recusa de sobrescrita e de incompatibilidade versão/tamanho.
- `tools/test_gs_capture_finalize.py`: V2/V3, corte na última referência,
  preservação do bruto e validação do tamanho do latch.
- Os 19 patches aplicam em sequência sobre `e563698` num índice temporário;
  a árvore resultante é idêntica ao HEAD do runtime (`f2894ba`). O aplicador
  reconhece os 19 patches já presentes. O clone externo está limpo.

### Execução real e replay

Aquisição de 40 s com ISO original e `DRA2_GS_BACKEND=gpu-shadow`, sem entrada,
encerrada pelo coletor (SIGTERM, sem crash observado). Não é medição de FPS.
O log contém `GsSetCrt: interlaced=1, videoMode=2, frameMode=0` e
`primeiro scanout 640x448 smode1=0x740804504`.

Captura completa V3: 58.611 registros, 180 latches e seis referências. Os
primeiros quatro latches precedem a configuração CRT e têm SMODE1=0; os 176
seguintes têm `0x740804504`. O replay Vulkan contabiliza 176 imagens e quatro
scanouts vazios, e `--scanout` exporta 640×448 no latch 180 com
`video_mode_source=captured-smode1`. Não usa hipótese NTSC.

O replay CPU passou nas seis referências com zero bytes diferentes. A GPU
divergiu nas seis, com 4.806.305 diferenças de bytes somadas entre referências.
O comparador exportou os seis pares e diferenças DISPFB. A imagem final de
scanout contém geometria/textura do cenário de abertura, mas não houve aceite
visual contra o jogo original. O roteiro sem entrada não comprova chegada ao
menu nem partida. Nenhum ganho de FPS é alegado nesta rodada.

Artefatos ignorados: `.cache/performance/parallelgs-smode1/`, incluindo as duas
capturas, metadados, logs, replays, checkpoint diffs e scanout PPM/PNG. Captura
usada no replay, SHA-256:
`fe96e6a580d554d190ec9987172701972ea1b68e23f5b8bc5c1ebb347cac7cff`.
Nenhum dado do jogo foi versionado.

## Próximo passo

A lacuna de SMODE1 foi resolvida para os modos TV cobertos e para o caminho
HLE efetivamente usado pelo jogo. Ainda falta diagnosticar e aceitar a imagem
GPU, ampliar as cenas e medir scanout válido em pares controlados. A conexão
à janela exige integração explícita de dispositivo/contexto e threads;
readback completo por quadro continua inadequado para o caminho ao vivo.
Sinais e promoção a backend autoritativo também permanecem pendentes.
