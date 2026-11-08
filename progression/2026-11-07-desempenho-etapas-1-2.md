# 2026-11-07: desempenho, etapas 1 e 2

Pedido: investigar a lentidão usando REA, dividir o trabalho, executar somente
as duas primeiras etapas, atualizar as docs e fazer commit. As duas etapas
foram medição e uma otimização pequena validada. O restante fica no
[plano de desempenho](../docs/desempenho.md).

## Etapa 1: perfil e contador de renderização

O Ghidra aberto confirmou `SLUS_210.67`, `r5900:LE:32:default`, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de `0x003a2460` mostrou envio DMA, troca dos buffers e incremento
do contador em `$gp+0x440` (`0x00538930`). O contador é uma medida de avanço da
rotina de renderização, não confirmação de cada quadro apresentado.

REA 6.8.0 foi usado pela CLI para identidade do ELF e por uma sessão MCP
persistente temporária para GDB. A primeira coleta de 40 amostras encontrou
`GSCpuBackend::Submit` em 23 pilhas do GameThread, `DrawTriangle` em 19 e
`WritePixel` como folha em 9. Isso direcionou a investigação ao GS por CPU.
A coleta seguinte, 60 amostras a cada 0,5 s, mediu **7,7899 avanços/s** do
contador durante a abertura. Nenhuma tecla foi enviada.

As hipóteses anunciadas foram trabalho repetido por pixel, acesso às texturas
e custo VU. A inspeção encontrou quatro chamadas `lround` por pixel bilinear;
3 das 60 folhas da segunda coleta estavam em `lroundf32`. O objetivo desta
rodada era remover uma parcela desse custo, mantendo a imagem.

Foi criado `tools/profile_game.py`, que inicia o jogo como filho do GDB por
REA, coleta pilhas e contador e salva evidências e hashes. Lançar como filho
evita a restrição Yama de attach encontrada na sessão anterior. `perf record`
foi bloqueado tanto para ciclos quanto para `cpu-clock:u`; o kernel não foi
reconfigurado. O comando efêmero `debug-with-gdb` recusou `source` pela
verificação da rota de saída; a coleta passou a usar o fluxo persistente
`start_gdb_session` → `gdb_console` → `close_gdb_session` suportado pelo REA.

Evidências locais, todas fora do git:

- `.cache/perf-rea-elf.json`:
  `ev_0240d92d620c69715f4206e13dbd69b9f8d042f9014d2c8511052662b876c962`.
  Objdump confirma o contêiner/entry, mas não reconhece R5900; a arquitetura
  vem do Ghidra.
- `.cache/performance/before/gdb_console.json`:
  `ev_2284b0a9fb0795939dadcf520e6673f55276b957b862bd55f1066f35e90f2ca9`.
- Coletas com contador: `.cache/performance/before-counter/`,
  `after-counter/` e `final-after/`; cada uma contém o registro completo REA
  em `gdb_console.json` e as pilhas em `samples.json`. IDs, na mesma ordem:
  `ev_07fbdd143751d14cc314ee9425ef9226732157cb1bc55f6a2e1201ccde0b5e20`,
  `ev_645e978f2a3f243a1aca3592c9642693c495a153ce2c3f0129352fa84636ebf8`,
  `ev_9527798cad6d9801d7255ade761d1a169904d8ad21934cbbc9b7666f43381ad3`.
- A ferramenta final também grava `identity.json` e `summary.json`, validada
  na coleta `final-after`. O executável anterior foi preservado em
  `.cache/performance/dra2-before`.

## Etapa 2: patch 0014 e validação

`0014-avoid-libm-bilinear-rounding.patch` mantém a interpolação original em
float, soma 0,5 em double e converte para inteiro antes do clamp. A mudança
elimina quatro chamadas libm por pixel filtrado sem desligar o bilinear.
A soma em double evita arredondamento prematuro perto de meias unidades.
O commit correspondente no clone `dra2-patches` é `ab09da6`.

Validação:

- `tools/check_gs_bilinear.sh`: **1.983.040 comparações** com a fórmula
  original, incluindo casos de borda e um milhão de entradas pseudoaleatórias.
- Benchmark com 80 triângulos, texturização e filtro bilinear pelo `Submit`
  real. Hash FNV-1a da VRAM inteira preservado: `1da861ea475b3918`.
- Uma variante ingênua com `float + 0.5f`, compilada somente em `.cache/`,
  falhou na comparação. O teste detecta a regressão numérica relevante.
- **45/45 testes** de GS/cache passaram, incluindo CLUT, formatos de pixel,
  cache e visibilidade de transferências.
- Os **14 patches** aplicaram em sequência em um índice temporário de
  `e563698`; o aplicador idempotente reconheceu os 14 como já aplicados.
- Sintaxe Python/Bash e `git diff --check` conferidos.

Cinco execuções alternadas antes/depois, sem compilação concorrente, deram
medianas de **1,24762 s → 1,07442 s** no benchmark: menos 13,9% de tempo,
mais 16,1% de vazão. O primeiro lote, contaminado pela compilação concorrente,
foi descartado para a comparação. O registro válido está em
`.cache/performance/bench-results.json`.

No jogo, as coletas de ~30 s deram **7,79 → 8,10 e 8,18 avanços/s** na
abertura, ganho observado de 4–5%. A coleta por debugger tem overhead e o
cenário não é uma partida; essas poucas execuções não são uma estimativa
estatística estável. O jogo continua lento e precisa das etapas seguintes.

`build/dra2` foi atualizado: o objeto do backend e `ps2_runtime` foram
recompilados e o comando de link gerado pelo Ninja foi executado com a
biblioteca do código recompilado já existente. O build global tinha mais de
duas mil ações do código gerado pendentes; essa recompilação ampla foi
interrompida para manter esta rodada restrita ao backend. Não foi alterado
nenhum arquivo de `generated/`, e não se declara aqui um rebuild completo.
O executável ligado foi usado nas duas coletas finais.

Não foi validada nesta rodada a navegação do menu ou a entrada em partida.
As capturas anteriores e a correção Lua/checkpoints permanecem como a referência
funcional anterior. Não houve alteração em override, timing do EE ou VU.

Fontes: [PS2Recomp: arquitetura e limitações de VU/GS](https://github.com/ran-j/PS2Recomp),
[semântica de lround](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/lround-lroundf-lroundl-llround-llroundf-llroundl).

## Ponto de parada

Etapas 1 e 2 encerradas conforme solicitado. Próxima etapa: replay determinístico
de primitivas reais para otimizar `WritePixel`/`SampleTexture`, comparando VRAM
integral e tempo. Uma arquitetura de renderização acelerada exige outra rodada;
não foi iniciada nesta sessão.
