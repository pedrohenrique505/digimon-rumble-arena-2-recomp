# paraLLEl-GS: coerência de escrita e comparação por checkpoint

Continuação do porte a partir da prova offline de 07/11. Foram preservadas as
alterações anteriores no working tree. O jogo ainda usa GS CPU; etapas 1 e 2
permanecem em progresso e a integração ao jogo ainda não foi iniciada.

## Identidade e investigação

Ghidra conectado à GUI confirmou `SLUS_210.67`, MIPS R5900 little endian,
SHA-256 `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de `0x003a2460` mostra escrita de PMODE, SMODE2, DISPFB1/2,
DISPLAY1/2 e BGCOLOR, alternância de buffers e incremento de `$gp+0x440`.
É observação estática, não prova de apresentação única de quadros.

REA 6.8.0 foi usado pela CLI `inspect-native-exports`, sem alterar registro MCP.
Evidence ID: `ev_5123ea7e012701c3bf310d62ee24731816d4785ff6a07accd0793deca81b021c`.
Confirmou identidade do ELF e ausência de tabela de símbolos dinâmicos. Não foi
feita nova coleta de execução REA/GDB nem alegação de desempenho com esse dado.
Registro e bundle completos: `.cache/parallelgs-20261108-elf{,-evidence}.json`.
O projeto aberto da GUI não foi acessado por headless.

## Implementação

- Escrita direta (registro 9) no candidato: flush, mapeamento sincronizado de
  VRAM, swizzle CPU existente e `end_vram_write`. Mapeia todos os 4 MiB;
  o custo síncrono pertence ao trabalho do replay.
- Upload nativo parcial: o teste pré-existente de um pixel CT32 passou no CPU
  e falhou no candidato, que devolveu quatro bytes zero. A revisão pinada usa
  `required_qwords = (RRW * RRH * bpp) / 64`; para 32 bits isso desativa a
  transferência. O adaptador passa uploads nativos host/local cujo tamanho não
  é múltiplo de 64 bits pelo caminho CPU sobre VRAM GPU sincronizada. Há também
  teste de três pixels CT24. O contador `native_upload_fallbacks` explicita o uso.
  Não foi alterado o checkout do candidato. Uploads IMAGE parciais dentro do
  fluxo GIF geral ainda precisam de cobertura/correção própria.
- `--checkpoints`: PPM e metadados por referência, com exportação excluída do
  tempo de trabalho. O formato DRA2GS02 não mudou.
- `compare_gs_checkpoints.py`: exige checkpoints com posições coincidentes e
  referências CPU exatas, exporta diferenças RGB e métricas. Não concede aceite
  visual automático; compara DISPFB bruto, sem deinterlace nem janela.
- FIFO local/host permanece recusado: a API do candidato consome palavras de
  16 bytes e preenche zeros após o fim; o runtime consome bytes parciais e
  retorna o tamanho efetivo. Um adaptador não pode simplesmente arredondar a
  solicitação sem preservar os bytes restantes e a semântica de nova transferência.

Fontes primárias consultadas:
[API pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_interface.hpp)
e [implementação pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_interface.cpp),
`init_transfer`, `read_transfer_fifo` e `map_vram_write/end_vram_write`.

## Validação

Build standalone CPU/Vulkan concluído na revisão pinada, pelo helper existente.
Os 11 casos de `test_gs_capture.py` passaram em cada backend. Incluem erro de
referência deliberado, framing inválido, upload de um pixel, upload CT24 ímpar,
escrita direta e upload GPU de quatro pixels seguido de escrita CT24 preservando
alfa; a checagem cobre os 4 MiB completos. O candidato continua retornando zero
quando há divergência de referência, para permitir investigação; o teste exige
que o contador de divergência a detecte.

O comparador foi exercitado com imagens idênticas, diferença de um pixel e
rejeição de referência CPU divergente. Python compilou sem erros e
`git diff --check` passou. O executável do jogo e o código gerado não foram
recompilados nesta continuação.

A captura longa histórica `structural/capture-menu/complete.gs` foi reproduzida:
170.183 registros, 660 latches e 22 referências. CPU: zero divergências em todas
as referências. Vulkan final: divergências nas 22, somando 8.540.894 bytes nas
referências. Esse fluxo não usa escritas diretas nem uploads nativos parciais
(`direct_writes=0`, `native_upload_fallbacks=0`); portanto as correções sintéticas
não eliminam as divergências já existentes dessa captura.

No último checkpoint, 231.693 de 286.720 pixels RGB diferem; erro absoluto médio
por canal 11,0276/255 e máximo 248. A inspeção lado a lado mostra composição
semelhante, com diferenças visíveis de linhas/detalhes. Isso não estabelece qual
rasterizador corresponde melhor ao PS2. Não houve aceite de equivalência visual.
Os tempos dessas execuções com exportação e trabalho concorrente não substituem
os cinco pares históricos de benchmark e não são medição de FPS.

Artefatos ignorados: `.cache/performance/parallelgs-20261108/`, incluindo
`cpu.json`, `gpu-final.json`, `cpu.checkpoints/`, `gpu-final.checkpoints/`,
`comparison-final/summary.json` e imagens de diferença. Nenhum dado do jogo foi
adicionado ao git. A infraestrutura foi registrada no commit `9b3e3a9`;
a prova, as correções do adaptador e a documentação foram reunidas em um
segundo commit. Build CPU/Vulkan e os 11 casos por backend foram repetidos
com sucesso antes de registrar essa entrega.

## Próximo ponto de retomada

Investigar a primeira divergência no checkpoint/latch 30 e separar diferenças
de rasterização das heurísticas de scanout. Cobrir upload IMAGE com word parcial
e implementar leitura FIFO com preservação de consumo parcial antes do adaptador
no jogo. A apresentação Vulkan, sinais SIGNAL/FINISH, coerência geral e seleção
de backend continuam pendentes; não há alegação de aceleração aplicada ao jogo.
