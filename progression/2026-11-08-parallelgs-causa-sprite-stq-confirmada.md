# Causa confirmada: amostragem STQ de sprites deslocada

A diferença do registro 38 foi explicada e corrigida no GS CPU. O pacote
original agora produz os mesmos 4 MiB nos replayers CPU e parallelGS. Isso
fecha esta ocorrência; não aprova a imagem completa nem promove GPU-shadow.

## Cadeia causal

O replay de `equal-state-sprite.gs`, preservando o prefixo igual até o registro
37, reproduziu novamente 64 bytes: 32 pares CPU/GPU 240/0 e 32 pares 95/0.
O mapeamento físico CT16 localizou todos nos pixels x=0..31, y=447.

`DrawSprite` interpolava STQ com `(pixel - origem + 0.5) / extensão`.
As coordenadas fornecidas pelo pacote já começam em meio texel e avançam um
texel por pixel. Na última linha, o acréscimo indevido transforma T=447.5 em
448.0. Nearest passa a buscar a linha seguinte da textura.

Leitura dos texels da VRAM do caso, com `GSMem::ReadCT32`, base 2240 e largura
10, confirmou para x=0 e x=1:

- linha 447: `0x7d000000`, RGB zero;
- linha 448: `0x00bffe80`, RGB (128,254,191).

A conversão RGB555 de (128,254,191) produz `0x5ff0`: exatamente os bytes
240 e 95 observados. Assim, a posição da amostra explica localização, valor
e desaparecimento da diferença após a correção. Não é apenas correlação de
contagem de bytes.

## Hipóteses e reprodução independente

Foram distinguidas amostragem, cobertura e coerência de textura. No sprite
original, deslocar T em -0.5 texel elimina os 64 bytes. Alterar TEX1 para zero mantém
o padrão original. Alterar TEST reduz a contagem mas não resolve a causa.

Um caso sintético usa upload novo de textura gradiente CT32 em região separada
do framebuffer, sem dados do jogo. Com STQ, origem de meio texel, Q=1 e nearest,
o primeiro pixel CPU lê (1,1) onde parallelGS lê (0,0). Controles UV e cor sólida
passam. Isso reproduz o defeito sem cache de uma renderização anterior e sem
formato CT16, blend, dithering ou teste alfa.

`tools/test_gs_sprite_sampling.py` exercita o frontend GIF real: três caminhos
(STQ, UV e sólido), duas origens e quatro modos de arredondamento, 24 casos.
Antes da correção, os quatro casos STQ com origem de meio texel falham, cada
um nos quatro pixels verificados: `04044080 != 00004080` no primeiro pixel.
Os mesmos 24 casos passam no parallelGS antes da correção CPU.

Correção de interpretação dos probes da sessão anterior: o pacote usa
PRMODECONT=0, e TME vem de PRMODE (0x1b). Alterar apenas os atributos de PRIM
no GIFtag não desativa sua textura. Aquele probe não sustentava exclusão da
hipótese de amostragem. Os novos probes alteram os registradores pelo endereço.
O probe correto sem textura expõe diferenças adicionais de cor no fluxo real;
não foi usado como prova de equivalência global do rasterizador.

## REA e Ghidra

REA/GDB executou o replay CPU com o mesmo caso sintético e parou na primeira
chamada a `GSCpuBackend::SampleTexture`, com DrawSprite/Submit na pilha.
Observação anterior: S=T=0.015625 (1/64), Q=1. Depois: S=T=0.0078125
(0.5/64), Q=1. A observação direta dos argumentos confirma o deslocamento.
Os dois processos observados foram explicitamente encerrados por `kill` no
script GDB, sem continuar o replay sob debugger.

Evidences completos em `.cache/performance/parallelgs-cause/`:

- `rea-gdb-before.json`: `ev_0a1e217618b4276e25ee4e50a06efb8a2a340a81d17729dae1577ab3999ebd3d`;
- `rea-gdb-after.json`: `ev_c5761103c643c209ac5cfab8d329da898aa8530418ab18788b13a29410e38be0`.

O replay CPU corrigido tem SHA-256
`334c51f879e6af3ad29b9f9f9169f1d56abbadb723004e34ae7dc0de65e8223d`.
As evidências GDB registram arquivo e argumentos no script; o identificador da
sessão isoladamente não estabelece identidade do inferior. A observação posterior foi readquirida após a retomada
do cliente e preservada integralmente antes de fechar a sessão. REA confirmou
`target_state=none_observed` no encerramento.

Ghidra confirmou SLUS_210.67, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
Não se atribui o pacote a uma rotina do jogo: a causa demonstrada está no
rasterizador nativo. A sessão GUI foi preservada. Não houve medição em PS2 real.

## Correção e validação

Patch 0022, commit local do runtime `576abee`: retira o meio pixel adicional
somente da interpolação STQ do sprite. O caminho UV conserva seu comportamento
anterior; precisão subpixel, interpolação UV fracionária, Q variável e equivalência
geral de rasterização não foram resolvidos nesta alteração.

O teste sintético foi versionado antes da correção (`60bbcee`). Após o patch:

- 24 casos de sprite passam em cada replayer;
- 16 casos de captura passam em cada replayer;
- 12 casos de profundidade e 12 de triângulo constante passam;
- regressão bilinear: 1.983.040 comparações, hash `1da861ea475b3918` preservado;
- build dos replayers e religação de `dra2` passaram;
- 22 patches aplicados sequencialmente sobre `e563698` em índice temporário
  produzem a árvore exata do HEAD `576abee`; o aplicador reconhece todos.

O sprite isolado e o **prefixo original inteiro até o registro 38** agora
coincidem nos 4 MiB, SHA-256 comum:
`85e0e048223d3a125a0b08bd9340b43f194c3c87fce2479f3a6629588cc3efcb`.
Resultados em `fixed-result.json`, prefixos/VRAM e logs na pasta local acima.
Não houve sobrescrita da VRAM GPU com referências CPU.

## Fontes e próxima etapa

Semântica corroborada pelo parallelGS pinado em
`cc6184af7e0c03da603045ca371ffa5dae9b0655`, especialmente
`gs/shaders/intersect.h::evaluate_barycentric_ij`, que avalia os atributos nas
coordenadas inteiras do pixel. O caso executado confirma a interpretação para
os sprites cobertos. Consulta primária online:
[PCSX2 GSSetupPrimCodeGenerator.all.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp),
snapshot SHA-256 `7c7a1143cab5b9dd362be602289aad8cf8e966a60ad9c9e3e93afeda62a293af`.
O caminho antigo `.x64.cpp` retornou 404; a consulta usou o arquivo atual `.all.cpp`.

A próxima investigação deve localizar a primeira diferença posterior ao registro
38. As referências CPU da captura antiga pertencem ao rasterizador anterior:
não reescrevê-las para declarar aceite do corrigido. Adquirir captura nova
quando for validar referências completas. Não houve captura, benchmark de FPS,
aceite visual de cena completa ou mudança de backend autoritativo nesta rodada.
