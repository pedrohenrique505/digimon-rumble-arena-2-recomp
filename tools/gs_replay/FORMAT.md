# Captura GS e replay experimental

Formato atual `DRA2GS03`, little endian, Linux x86-64; leitura compatível com `DRA2GS02`. Cada registro tem dois `uint32_t`
(tipo e tamanho), seguidos pelo payload. Limite da captura: 512 MiB. Os dados
são derivados do jogo: somente em `.cache/`, nunca no git.

| Tipo | Payload |
| --- | --- |
| 0 | VRAM inicial, exatamente 4 MiB; primeiro registro |
| 1 | Pacote GIF aceito pelo frontend; inclui os uploads IMAGE |
| 2 | Dois u64: endereço do registrador, valor |
| 3, 4 | Quatro u64 de BITBLTBUF/TRXPOS/TRXREG/TRXDIR, seguidos por registro 4 com upload nativo |
| 5 | V2: sete u64 PMODE, SMODE2, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, BGCOLOR. V3: os mesmos sete seguidos por SMODE1 (oito u64); evento de latch |
| 6 | Referência de 4 MiB de VRAM, a cada 30 latches |
| 7 | Dois u32: índice de contexto, cor do clear HLE |
| 8 | Um u32: consumo local/host solicitado |
| 9 | Seis u32: escrita direta psm/base/bw/x/y/valor |
| 10 | Reset de contexto, sem payload |
| 11 | Um u32: `fegetround()` do host; valores nativos da plataforma |

A captura serializa comandos e referências pelo mutex de estado do GS. Isso
altera desempenho: nunca usar a aquisição para medir FPS. O modo de arredondamento
é necessário porque o rasterizador herda FE_TOWARDZERO durante XGKICK.
`capture_gs.py` preserva o arquivo bruto e produz um prefixo até a última
referência completa. O descarte da cauda fica nos metadados. EOF no meio de um
registro é erro no replay. Cada backend recebe a mesma VRAM inicial e o mesmo
fluxo, sem executar EE/VU/IOP. A checagem de referências fica fora do tempo de
trabalho; sincronizações necessárias são medidas separadamente.

O candidato usa GIF antes de `GSRasterBackend::Submit`. O protótipo faz scanout
com `GSInterface::vsync`; não apresenta numa janela. Exporta VRAM e uma imagem
PPM de 640×448 do DISPFB selecionado, antes das heurísticas de apresentação do
frontend CPU. Upload, submissão e scanout estão no tempo de trabalho; o término
da GPU é incluído em `sync_seconds`. Não somar tempos de validação às médias de
execução. O clear HLE usa a rotina CPU sobre VRAM mapeada e mede essa volta completa.
Leituras local/host usam adaptador com buffer residual: o candidato entrega
qwords de 16 bytes, o consumidor pede bytes arbitrários e a cauda sub-qword é
reconstruída da VRAM sincronizada, com verificação contra o backend CPU.
Uploads IMAGE no fluxo GIF com bits não múltiplos de 64 usam o mesmo fallback
CPU dos uploads nativos. Escritas diretas usam
`WriteVram` CPU sobre VRAM GPU mapeada, após flush, seguidas de `end_vram_write`.
Uploads nativos com tamanho em bits não múltiplo de 64 usam transferência CPU
sobre esse mesmo mapeamento para preservar o último word parcial. O caminho
GIF IMAGE geral ainda precisa de cobertura para esses tamanhos.
`--checkpoints` exporta PPM e posição de evento/latch em uma pasta nova por
referência, com custo incluído somente em `validation_seconds`. O formato do checkpoint permanece inalterado.

Snapshots VU `DRA2VU01` são nativos e privados à mesma revisão/build: cabeçalho
com sizeof(interpreter), tamanhos de código/dados e orçamento; estado completo
do interpretador sem ponteiros de host, código/dados iniciais, registradores e
dados finais e hash/tamanho do GIF emitido. Incluem pipelines e XGKICK pendente.
Não são um formato de distribuição ou entrada não confiável. O replay não
desenha: constrói e verifica os bytes GIF, comparando também memória e
registradores. Uma execução aquece; cinco são medidas. As primeiras 16 fatias
VU1 que emitem GIF são amostradas, sem promessa de representar as mais caras.
EOF, tamanho incompatível e divergências são erros. A integração GPU ao jogo
continua fora do escopo desta prova.

### Exportação de scanout e estado ausente

`DRA2GS02` não contém SMODE1. A chamada a `vsync` sem esse estado não prova
scanout válido; na revisão pinada, LC=0 produz `Unknown video format` e imagem
vazia. `--scanout-ntsc` é uma hipótese diagnóstica explícita (LC=32, CMOD=2),
não uma extensão do formato nem recuperação do registrador original.
Exporta somente o último latch para `.scanout.ppm` e `.scanout.json`, sem
alterar a exportação DISPFB existente. Dimensões dependem de DISPLAY/SMODE2;
o JSON inclui dimensões internas/do modo, latch, interlace e a origem da
hipótese. Ausência de imagem é erro. O readback síncrono final fica em
`scanout_export_seconds`; a execução de scanout continua em `work_seconds`.
Essa rodada não deve substituir a matriz histórica de benchmark.

### Captura V3: estado de vídeo preservado

O cabeçalho `DRA2GS03` exige latch de 64 bytes; V2 exige 56 bytes. O replay e
o finalizador rejeitam misturas de versão e tamanho. Os demais eventos são
idênticos. O writer atual produz V3; não reescreva o cabeçalho de capturas V2
para tentar recuperar SMODE1 ausente.

`--scanout` exige V3 e usa SMODE1 capturado, registrando
`video_mode_source=captured-smode1` e o valor do registrador no JSON.
`--scanout-ntsc` continua sendo uma hipótese explícita para diagnóstico legado;
as duas opções são mutuamente exclusivas. Cada `vsync` Vulkan é contabilizado
em `scanout_images` ou `empty_scanouts`, mesmo sem exportação. A contagem de
imagens não garante conteúdo correto nem mede apresentações únicas. Na CPU
esses dois contadores são zero, pois não há chamada ao vsync Vulkan.

O HLE atual preenche SMODE1 para CRT IDs `0x02`/`0x03` (NTSC/PAL); não emula
todos os modos BIOS. `sceGsResetGraph` delega ao mesmo HLE nos modos 0 e 5.
Capturar o valor derivado pelo HLE não prova equivalência ao BIOS original.
