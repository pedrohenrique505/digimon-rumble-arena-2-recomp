# Localização das divergências por página de VRAM

Continuação do scanout V3. O candidato ainda não foi aprovado visualmente e
continua shadow; nenhuma mudança na janela ou no timing EE/VU nesta rodada.

## Entrega e validação

O commit `75f5018` acrescenta aos JSONs de `--checkpoints` as contagens de
bytes diferentes nas 512 páginas físicas de 8 KiB. Isso permite separar a
localização de diferenças da exportação RGB limitada ao DISPFB. A soma das
páginas é verificada contra a diferença total. Não implica que uma página
pertença exclusivamente a um framebuffer ou textura.

Build dos dois replayers passou (warning herdado de `std::is_pod`). Os 16 casos
CPU e 16 Vulkan passaram. Os testes verificam tanto 512 páginas sem diferença
quanto um byte divergente na última página, com fechamento da soma.

Replay Vulkan da mesma captura V3 usada na rodada anterior:
`.cache/performance/parallelgs-smode1/capture-fixed/complete.gs`.
Saída nova em `.cache/performance/parallelgs-pages/`, com checkpoints e scanout.
As seis somas coincidem com as diferenças históricas; a VRAM final permanece
idêntica byte a byte ao resultado GPU anterior. Não houve correção de imagem.

A primeira referência (registro 421, latch 30) já difere em 246 páginas,
entre os índices 60 e 349. Portanto, a divergência antecede esse checkpoint;
não é correto chamar o latch 30 de primeiro comando divergente. Há páginas
inteiras sem diferença entre esses índices. O diagnóstico ainda não identifica
a causa nem estabelece qual renderizador corresponde ao hardware original.

## REA e Ghidra

Ghidra confirmou o ELF `SLUS_210.67`, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de `FUN_003a2460` confirma escritas dos dois circuitos
DISPFB/DISPLAY e alternância de buffers (`uGp00000428 ^= 1`). Isso reforça a
necessidade de distinguir a exportação bruta do resultado de apresentação.
É evidência estática, sem comprovação de qual ramo foi executado.

REA inspecionou via GNU objdump o símbolo nativo `saveDisplay` do replay Vulkan
anterior ao rebuild: o teste de PMODE e a seleção de DISPFB2 ou DISPFB1 aparecem
no assembly, seguidos por chamadas a `GSCpuBackend::ReadVram`. O exportador é
uma leitura bruta por CPU, não a composição do scanout Vulkan.
Evidence pai: `ev_2d5a25a65b7f8d9f6d9ab453152ce5507a7eae35b0778629c0fb0b705c3bee2a`.
Projeção: `ev_0c035f3b385a98a1e07a50a78a31784c344bf37e87e3b3efd4fbefb5d836d65a`.
SHA-256 do binário inspecionado:
`77f9b7c7f35e38183948f284d1769e3054a0523ef0ee612191072ee787d34180`.
A operação terminou sem truncamento ou alteração do artefato. Não foi aberta
sessão nativa REA; a sessão GUI Ghidra existente foi preservada.

## Retomada

Próximo diagnóstico: reproduzir prefixos anteriores ao registro 421 nos dois
backends, comparando a VRAM ao fim de cada prefixo para isolar o primeiro
comando divergente. Uma referência tipo 6 marca o estado CPU capturado;
não deve ser usada para sobrescrever a VRAM GPU e ocultar diferenças.
Depois de isolar a primitiva/transferência, criar caso sintético e conferir a
semântica GS antes de alterar rasterização. Aceite visual, cenas adicionais,
medição de scanout válido e apresentação direta na janela continuam pendentes.
