# Primeira divergência isolada e corrigida

Continuação da integração paraLLEl-GS. O CPU permanece autoritativo e o
candidato continua shadow. Não houve alteração no timing EE/VU nem integração
da janela. Não houve benchmark novo ou aceite visual.

## Reprodução e redução

Uma varredura sequencial de prefixos completos da captura V3 anterior encontrou
a primeira diferença no registro **32** (índice começando em zero, incluindo a
semente). É um evento GIF de 464 bytes. Até o registro 31, a VRAM CPU/GPU era
idêntica. Cabeçalho/payload de upload nativo devem ser tratados como unidade;
prefixos sem referências retornam 2 mesmo quando exportam VRAM corretamente.
Não foi usada busca binária: uma escrita posterior pode apagar a divergência.

Artefatos locais em `.cache/performance/parallelgs-first-diff/`: `probe.py`,
`first-difference.json`, `before.gs`, `after.gs`, scripts de redução e VRAM.
SHA-256 de `after.gs`:
`22af7da12a0e625cf28097792c9b7567f731e2cddf30a923013ce98c3b51bdf3`.

A primeira redução eliminou configuração relevante e expôs outro defeito:
a CPU escrevia profundidade com TEST.ZTE=0. Esse caso foi corrigido e testado,
mas sua correção **não** mudou a diferença do prefixo original, onde ZTE=1.
Por isso, a segunda redução preservou como predicado o par de bytes observado
no original (CPU 123, GPU 125), em vez de aceitar qualquer diferença.
Esse caso reteve os registros 0, 14, 22, 28, 30 e 32.

No prefixo original, os únicos pares divergentes eram 123/125 (286.711 bytes)
e 127/128 (65.086 bytes). Desligar apenas IIP no PRMODE removeu todos os pares
123/125, deixando os de profundidade. Os vértices tinham RGBA e Z constantes.
Com pesos baricêntricos float, sobretudo sob FE_TOWARDZERO, a soma pode perder
uma unidade; a modulação de alfa amplifica a diferença. Atributo com gradiente
zero deve manter seu valor, sem depender da soma aproximada dos pesos.

## Correções em commits pequenos

- `62aeecd`, patch 0020: TEST.ZTE=0 deixa a profundidade intacta, independentemente
  de ZTST. O teste percorre o frontend GIF real, com framebuffer e Z separados.
  Falhou antes com `unexpected depth write with TEST=0`; passa nos 12 casos.
  A implementação concorda com
  [PCSX2 SW](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/SW/GSRendererSW.cpp)
  (`zm = ZMSK || ZTE == 0 ? 0xffffffff : 0`) e paraLLEl-GS pinado
  (`state_is_z_sensitive`). Isso não é uma observação de hardware PS2.
- `f3d8d72`, patch 0021: mantém Z constante exatamente e usa a cor do vértice
  quando os quatro canais RGBA são constantes, inclusive em Gouraud. Preserva
  a fórmula anterior dos gradientes variáveis e o modo de arredondamento do
  chamador. Não pretende tornar o rasterizador CPU inteiro equivalente ao GS.
  Teste sintético com área não potência de dois, coordenadas subpixel, quatro
  modos de arredondamento e três cores: 12 casos. Com a fonte anterior limpa,
  o teste corrigido falha com `constant RGBA changed in rounding mode 0`.
  Framebuffer e profundidade ficam em regiões físicas separadas.

Após ambos os patches, os 4 MiB do prefixo original até o registro 32 são
idênticos entre CPU e GPU. SHA-256 comum:
`85e0e048223d3a125a0b08bd9340b43f194c3c87fce2479f3a6629588cc3efcb`.
Resultados em `.cache/performance/parallelgs-constant-fixed/first-*`.
O fluxo original foi preservado; não se substituiu a VRAM GPU por uma referência.

## Ferramentas e fontes

REA inspecionou `GSCpuBackend::WritePixel` no executável de teste após o patch
0020, antes do patch 0021, por disassembly GNU objdump. Evidence pai
`ev_211fc44054bf31c01993f94d05ebd08b1285bed9acd2dd0a35bbe4ca3be47362`,
projeção `ev_b14c79d607d662e8360b29ab0261f91c7af786d3667da7bdb9145ecf3136f5a2`.
Binário SHA-256:
`68702a23744e6a852dae593fb726230daa2aa6804b4846494482ce7e0846be03`.
Observação completa, sem truncamento ou mudança do artefato, salva em
`.cache/performance/parallelgs-first-diff/rea-depth.json`. Evidência estática;
os testes fornecem a observação de execução. Não foi aberta sessão nativa REA.

Ghidra confirmou novamente SLUS_210.67, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A inspeção de `FUN_003a2340`, chamada no início da submissão já estudada,
não identificou o produtor do pacote do registro 32. Não se atribui o pacote
a essa rotina nem a um microprograma VU específico com essa evidência.

O snapshot local do fonte PCSX2 SW consultado tem SHA-256
`61016efbbd3b3ef60b9df9f9fc5b658f16d30ab525d592f256699a1c357b3570`.
O link master é mutável; a comparação documentada refere-se a esse snapshot.

## Validação e captura nova

Build dos replayers e `dra2`: passou. Testes: 12 GIF de profundidade, 12 de
triângulo constante, 16 de captura em cada backend e scanout Vulkan NTSC/PAL.
A regressão bilinear manteve as 1.983.040 comparações e o hash anterior de VRAM
`1da861ea475b3918`. Os 21 patches aplicam sequencialmente sobre `e563698` em
índice temporário e produzem árvore idêntica ao HEAD do runtime (`8784123`).
O aplicador reconhece todos; o clone externo está limpo.

Capturas antigas continuam intactas e preservam o resultado da CPU anterior.
Não devem ser reescritas para declarar equivalência ao rasterizador corrigido.
Nova aquisição de 40 s, sem entrada, usando a ISO original e gpu-shadow:
`.cache/performance/parallelgs-constant-fixed/capture/`. Encerramento pelo
coletor com SIGTERM (-15); sem crash observado. Não mede FPS nem comprova menu
ou partida. SHA-256 da captura completa:
`9c46301a57200e850f0e11802aa8692844df455a19dc19965c6d170976ebb2d5`.

O prefixo completo tem 65.405 registros, 180 latches e seis referências.
O replay CPU passa nas seis com zero diferenças. A GPU ainda diverge nas seis;
os totais de bytes por checkpoint são 4.675, 52.929, 4.582, 4.647, 1.629.072 e
1.504.227. Esses valores pertencem à captura nova, com progressão distinta;
não são um experimento controlado de ganho contra a captura anterior.

Vulkan contabiliza 176 imagens e quatro scanouts vazios iniciais. O último
scanout exportado é 640×448, com `video_mode_source=captured-smode1`.
O comparador executou nos seis checkpoints e preservou os diffs em `comparison/`.
Não houve aprovação visual contra o original.

## Retomada

A primeira divergência do fluxo anterior está resolvida. A próxima investigação
deve localizar a primeira diferença restante na captura nova, preservando o
tipo de diferença e os estados de desenho durante a redução. Há diferenças
RGB e de VRAM ainda não explicadas. Depois vêm aceite das cenas e apresentação
direta da imagem Vulkan na janela; shadow não é backend GPU autoritativo.
