# Etapas estruturais 1 e 2: diagnóstico e prova Vulkan

Implementação parcial do roadmap, sem integrar GPU ao jogo (etapa 3).

## Resultado principal

Cinco pares alternados do mesmo fluxo real, com 660 latches de GS:

| Backend | Mediana do replay, trabalho + sincronização |
| --- | ---: |
| CPU | 49,3682 s |
| paraLLEl-GS / Vulkan | 1,3253 s |

**Ganho de 37,25× no replay GS isolado. Não é ganho de FPS do jogo.**
A captura tem 170.183 registros e 22 referências de VRAM. Os cinco replays
CPU coincidiram byte a byte com todas as referências. O candidato renderiza o
fundo e a geometria 3D capturados, mas diverge da referência CPU nos 22
checkpoints; a equivalência visual ainda não está aprovada. O replay faz
scanout offline, sem apresentação numa janela. Inicialização/semente e exportação
de imagens ficam fora do tempo de replay; trabalho e sincronização estão incluídos.

## Entregue

- Patch 0015: contadores exclusivos EE/VU/GS/IOP/apresentação, modo diagnóstico
  sem rasterização, captura GS com VRAM inicial e referências e replay VU.
- Ferramentas reproduzíveis em `tools/`: captura com roteiro de entrada,
  benchmark sem debugger, build/replay CPU/Vulkan e testes do formato.
- REA/GDB: coleta de atribuição arquivada em `.cache/performance/structural/rea`.
- Ghidra: ELF confirmado e contador de renderização validado em `0x00538930`,
  incrementado por `FUN_003a2460`. Hooks cobrem chamadas e retomadas no scheduler.
- Correção do experimento: registrar FE_TOWARDZERO herdado do VU durante GIF.
  Sem isso, até o replay CPU divergia. A aritmética do jogo não foi alterada.
- Sete testes de captura passaram, incluindo upload nativo e rejeição de truncamento
  e referência divergente. Benchmark bilinear preservou hash `1da861ea475b3918`
  em 1.983.040 comparações.
- As 16 amostras VU passaram em registradores, memória e GIF emitido, cinco
  repetições cada. Todas têm o mesmo hash de código; não representam a partida.

## Medição do jogo

Vinte execuções sem debugger, cinco de cada variante, mesmo binário e memory
card inalterado. Análise final pelos quadros lógicos 200–260; resumos exploratórios
por tempo de parede foram preservados separadamente.

| Variante | Mediana de submissões/s | Mediana do intervalo | p95 dos intervalos, mediana entre execuções |
| --- | ---: | ---: | ---: |
| CPU com contadores | 10,03 | 99,31 ms | 103,59 ms |
| Tracing diagnóstico ligado | 9,84 | 100,07 ms | 105,99 ms |
| Sem rasterização, imagem inválida | 23,26 | 42,74 ms | 43,67 ms |
| Sem contadores de subsistema | 10,73 | 92,94 ms | 95,57 ms |

A dispersão é relevante: sem rasterização variou de 22,69 a 47,42 submissões/s.
O resultado não estabelece um teto estável para todas as cenas. A velocidade de
simulação mediana foi 0,167× com contadores e 0,388× sem rasterização. O tracing
comparado é DRA2_TRACE_GS/RPC; os logs compilados permanecem ligados. A diferença
das medianas com/sem contadores é cerca de 7%, com dispersão sobreposta.
Não comparar diretamente com os 8,1–8,2/s da coleta anterior por debugger.

Custos CPU exclusivos medianos na janela de baseline, em ms de CPU por segundo
real: GS 647,4; VU 195,4; EE/dispatcher 165,6; IOP 52,9; apresentação 58,6.
Há múltiplas threads e overhead dos relógios; não interpretar isso como percentuais
de um único núcleo. Swaps da janela incluem quadros repetidos, não FPS único.

## Decisão e pendências

Manter paraLLEl-GS como candidato: API standalone e ganho GS justificam a prova.
Não iniciar a integração da etapa 3 nesta rodada. A aceitação das etapas 1 e 2
continua parcial: falta aprovar as diferenças visuais, validar cenários estáveis
de menu/navegação/partida e medir apresentações únicas. O roteiro de 90 s foi
capturado, mas não comprova sozinho todos esses estados. Também falta representar
os VUs caros e auditar feedback de VRAM nas outras cenas. Não há alegação de
60 FPS ou de aceleração já aplicada ao jogo.

Dados fora do git: `.cache/performance/structural/measure-steady/summary-guest-window.json`,
`replay-menu/summary.json`, `replay-menu/results.json`, `capture-menu/metadata.json`
e `vu-validation.json`. Capturas, VRAM e imagens continuam em `.cache/`.
Os arquivos novos e o patch estão no working tree, sem commit nesta sessão.
Instruções e avaliação do candidato: [docs/gs-acelerado.md](../docs/gs-acelerado.md).

## Estado consolidado no roadmap

As etapas 1 e 2 estão **em progresso**; as etapas 3, 4 e 5 estão **não iniciadas**.
Os trabalhos concluídos estão marcados individualmente em [NEXTSTEPS.md](../NEXTSTEPS.md).
A infraestrutura de replay VU é preparação da etapa 1, não aceleração da etapa 4.

O build desta rodada foi incremental: o runtime foi recompilado e `dra2` foi
religado com a biblioteca gerada existente. A tentativa de build global encontrou
cerca de duas mil ações antigas pendentes e foi interrompida; não foi validado
um rebuild completo do código gerado. A sessão também encontrou quota de `/tmp`:
dados ASTC opcionais e um build duplicado recém-criados foram removidos; o helper
reproduzível passou a buscar apenas as dependências necessárias em `.cache/`.
