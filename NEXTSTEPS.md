# Próximos passos: ganhos estruturais de FPS

Prioridade atual: remover os custos dominantes de renderização e execução VU.
O objetivo é alcançar velocidade de jogo correta, com imagem e entrada
preservadas. O patamar de trabalho é 60 quadros/s (16,67 ms/quadro), sujeito à
confirmação da cadência original de cada cena. Isso é uma meta, não um resultado
já demonstrado. A entrada em partida ainda precisa ser validada.

A referência histórica anterior a esta rodada é o patch 0014: cerca de 8,1–8,2 avanços/s do contador de
renderização na abertura. Esse contador não mede quadros únicos apresentados.
O ganho de 16% do benchmark bilinear não representa ganho equivalente no jogo.
Medições e comandos existentes: [docs/desempenho.md](docs/desempenho.md).

## Estado do roadmap

Legenda: **Concluída** = entrega verificada; **Em progresso** = há implementação,
mas falta cumprir o aceite; **Não iniciada** = implementação ainda não tocada.
Nos checklists, `[x]` marca trabalho concluído; `[ ]` mantém trabalho pendente,
com o estado indicado quando já existe implementação parcial.

| Etapa | Estado | Entregue | Falta para concluir |
| --- | --- | --- | --- |
| 1 — Medição e teto | **Em progresso** | Contadores exclusivos, modo sem rasterização, captura/replay VU, 20 execuções sem debugger | Estabilizar/comprovar todos os cenários, apresentações únicas, carregamentos e feedback de VRAM |
| 2 — Prova GS acelerado | **Em progresso** | Avaliação paraLLEl-GS, captura real, replay CPU/Vulkan e ganho GS de 37,25× em cinco pares | Aprovar equivalência visual e concluir cobertura e custos da apresentação |
| 3 — Integrar GPU ao jogo | **Em progresso** | Costura GSGifSink (patch 0016), adaptador shadow executado no jogo (60/120 s, sem crash), seleção DRA2_GS_BACKEND | Apresentação GPU na janela, sinais a partir da GPU e promoção a backend autoritativo |
| 4 — Acelerar VU1 | **Não iniciada** | Instrumentação/replay preparatório pertence à etapa 1 | Inventário dos programas caros, substituição/recompilação e validação diferencial |
| 5 — Validar partida e orçamento | **Não iniciada** | Nenhuma validação de partida nesta rodada | Partida completa, áudio/entrada, cadência original e orçamento de quadro |

Nenhuma das cinco etapas estruturais está integralmente concluída. Os itens
concluídos abaixo registram as entregas efetivas, sem antecipar seu aceite.
A rodada histórica de diagnóstico e otimização bilinear (patch 0014) está
**concluída**, mas não corresponde às etapas 1 e 2 deste roadmap estrutural.

## Resultados já verificados

| Experimento | Resultado | Limite da conclusão |
| --- | --- | --- |
| Replay GS, cinco pares do mesmo fluxo | CPU 49,3682 s → Vulkan 1,3253 s; **37,25×** | Só GS offline; não é ganho de FPS do jogo |
| Referência CPU da captura maior | 22 checkpoints de VRAM iguais byte a byte em cada um dos cinco replays | Valida captura/reprodução CPU, não equivalência GPU |
| Replay Vulkan | Fluxo de 660 latches reproduzido; scanout válido não comprovado na medição histórica | Há diferenças nos 22 checkpoints; imagem ainda não aprovada; sem swap de janela |
| Jogo, quadros 200–260, cinco execuções por variante | 10,03 submissões/s com contadores; 23,26/s sem rasterização | Imagem inválida sem desenho; dispersão relevante; não é FPS único apresentado |
| Replay VU | 16 amostras verificadas em registradores, memória e GIF; cinco repetições medidas por amostra | Mesmo hash de código nas 16; não cobre VUs caros da partida |
| Testes de captura/replay | 11 casos passaram em cada backend CPU/Vulkan | Uploads parciais, escrita direta, preservação de alfa, divergência e framing |
| Regressão bilinear | 1.983.040 comparações; hash de VRAM preservado | Confirma que o diagnóstico não alterou essa referência |

Resultados completos: [registro da rodada](progression/2026-11-07-gs-vulkan-replay-e-teto.md).
Comandos e limites: [diagnóstico e replay acelerado](docs/gs-acelerado.md).

## Continuação de 08/11

O adaptador de replay agora cobre escrita direta de VRAM com sincronização e
fallback para upload nativo com word parcial. Há exportação e comparação visual
por checkpoint, além de testes CPU/Vulkan. A captura maior continua divergindo
na imagem: nenhuma aprovação visual foi declarada.
Leitura FIFO com consumo parcial e uploads IMAGE parciais foram concluídos no replay.
A etapa 3 começou: o shadow foi religado e executado no jogo sem crash, com o
sink serializado e estatísticas no stderr. Apresentação GPU e promoção a
backend autoritativo continuam pendentes.
Evidências: [diário da continuação](progression/2026-11-08-parallelgs-coerencia-e-checkpoints.md)
e [shadow executado](progression/2026-11-08-parallelgs-shadow-executado.md).

## Sequência de implementação

### 1. Criar uma medição confiável e descobrir o teto de ganho — Em progresso

- [x] **Concluído:** confirmar o ELF no Ghidra e validar `FUN_003a2460` e o
  contador em `0x00538930`. Cobrir chamadas diretas e retomadas pelo scheduler;
  conferir correspondência um a um entre hooks e contador.
- [x] **Concluído:** coletar atribuição com REA/GDB e preservar evidências.
- [x] **Concluído:** implementar custos CPU/parede exclusivos de GS, VU,
  EE/dispatcher, IOP e apresentação, subtraindo filhos para não somar GS novamente
  como VU inclusivo. Verificar ordem dos relógios e distinguir overhead de espera.
- [x] **Concluído:** medir sem debugger e sem compilação concorrente, com hashes,
  máquina, revisão, configuração e verificação de memory card inalterado.
  Foram 20 execuções, cinco por variante, em ordem alternada.
- [x] **Concluído:** registrar intervalos mediano/p95 de submissão e velocidade
  de simulação pelo relógio EE. Analisar os mesmos quadros lógicos 200–260;
  preservar os resumos exploratórios por tempo de parede separadamente.
- [x] **Concluído:** implementar `PS2X_NO_RASTER=1`, omitindo primitivas e clears
  por pixel, mantendo consumo de comandos, uploads, CLUT e registros/sinais.
- [x] **Concluído:** capturar entradas/saídas VU1 com pipelines e XGKICK pendente;
  reproduzir isoladamente com cache de instruções preparado, verificando registros,
  memória e tamanho/hash do GIF emitido, sem desenhar no GS.
- [x] **Concluído:** comparar tracing diagnóstico `DRA2_TRACE_GS/RPC` ligado e
  desligado, com o mesmo binário/otimização, em cinco execuções de cada variante.
  Comparar também contadores de subsistema ligados/desligados. Os logs compilados
  do runtime continuam ligados; isso não é um build sem `AGRESSIVE_LOGS`.
- [ ] **Em progresso:** fixar e comprovar abertura, menu parado, navegação e
  partida como cenários reproduzíveis. Há coleta do aviso de autoload e fluxo
  de 90 s com roteiro/horários de entrada; o roteiro sozinho não valida todos
  os estados nem a entrada em partida.
- [ ] **Em progresso:** separar apresentações únicas de swaps repetidos da
  janela; registrar pausas de carregamento e ampliar a análise de aquecimento
  e dispersão. Os intervalos atuais medem submissões, não FPS único apresentado.
- [ ] **Em progresso:** auditar leituras/feedback de VRAM que invalidem o
  experimento sem desenho em cada cena. O formato registra consumo local/host
  e escrita direta, ausentes nas capturas usadas; isso não exclui feedback
  textura/framebuffer. Não há teto estável demonstrado para todas as cenas.
- [ ] **Pendente:** ampliar o replay VU a programas e entradas representativos
  das cenas caras. As primeiras 16 amostras não fazem esse inventário.

**Entrega parcial:** tabela de custo por subsistema e experimento sem rasterização
já existem. **Aceite restante:** cenários comprovados e teto suficientemente
estável para decidir o orçamento. Não usar o modo sem desenho para alegar FPS
jogável. Se o custo restante continuar alto, investigar EE/VU/IOP antes de
apostar só na GPU.

### 2. Provar a integração de um GS acelerado existente — Em progresso

- [x] **Concluído:** avaliar paraLLEl-GS/Vulkan: API, dependências pinadas, licença
  declarada, requisitos e GPU local. Biblioteca standalone compilada e executada
  na Radeon Renoir/RADV. Alternativa PCSX2 avaliada por seus acoplamentos, sem
  extrair ou medir seu renderizador.
- [x] **Concluído:** capturar fluxo real de GIF/registradores, uploads e eventos
  de latch/reset, VRAM inicial e referências. Dados do jogo ficam em `.cache/`,
  fora do git; ferramentas e descrição do formato ficam no repositório.
- [x] **Concluído:** tornar o replay CPU fiel ao estado de arredondamento do
  host. O formato inicial falhava por perder FE_TOWARDZERO durante XGKICK;
  o formato corrigido passou nas referências, sem alterar a aritmética VU/GS.
- [x] **Concluído:** reproduzir o mesmo fluxo nos backends CPU e Vulkan,
  comparando VRAM e exportando imagens. Fronteira escolhida: GIF após arbitragem,
  **antes de `GSRasterBackend::Submit`**, que já recebe primitivas decodificadas.
- [x] **Concluído:** medir cinco pares alternados de replay da captura maior,
  com trabalho e sincronização. Inicialização/semente, validação e exportação
  de imagens são discriminadas; não fazem parte do ganho de execução anunciado.
- [x] **Concluído:** registrar decisão de manter paraLLEl-GS como candidato,
  considerando o ganho de 37,25× e a API já exercitada. Não integrar ao jogo
  antes da aceitação visual. Dependências e limites estão em `docs/gs-acelerado.md`.
- [ ] **Em progresso:** investigar e quantificar as diferenças visuais CPU/GPU;
  decidir quais são diferenças de precisão/rasterização e quais são defeitos,
  conferindo uma referência do jogo original quando necessário. A comparação
  já existe, mas **a equivalência visual não foi aprovada**.
- [ ] **Em progresso:** demonstrar o aceite da cena completa, incluindo texturas,
  CLUT, blend, profundidade e framebuffer nas cenas selecionadas. O fluxo
  capturado já reproduz fundo e geometria 3D, mas não valida toda a cobertura.
- [ ] **Pendente:** completar a medição da apresentação. O protótipo executa
  scanout para imagem Vulkan, sem swap numa janela; a conexão à janela do jogo
  será implementada na etapa 3. Não confundir esses custos.
- [x] **Concluído no replay:** adaptar escrita direta de VRAM com flush,
  mapeamento sincronizado e swizzle CPU; validar preservação de alfa após upload GPU.
- [x] **Concluído no replay:** preservar uploads nativos com word parcial usando
  fallback CPU sincronizado e contador explícito.
- [x] **Concluído no replay:** adaptar consumo local/host preservando bytes parciais e
  tamanho efetivamente consumido, com buffer residual e cauda sub-qword reconstruída.
- [x] **Concluído no replay:** cobrir uploads IMAGE parciais no fluxo GIF geral via
  fallback CPU sobre VRAM sincronizada; falta ampliar a cobertura em capturas reais.
  O clear HLE tem fallback CPU com ida/volta de VRAM contabilizada.

**Entrega parcial:** prova executável acelerada e ganho múltiplo GS confirmados.
**Aceite restante:** aprovação visual, cobertura e custos ainda ausentes.
Não escrever um GS do zero nesta etapa nem chamar o speedup do replay de
speedup do jogo.

### 3. Integrar o backend escolhido ao jogo — Em progresso

- [x] **Concluído (shadow):** implementar o adaptador de GIF pós-arbitragem para
  o paraLLEl-GS, antes da decodificação em primitivas (patch 0016 `GSGifSink` +
  `runtime/parallel_gs/`). O CPU continua autoritativo; a GPU espelha o fluxo.
- [x] **Concluído:** adicionar seleção explícita CPU/GPU (`DRA2_GS_BACKEND=cpu`
  padrão ou `gpu-shadow`, build `DRA2_ENABLE_PARALLEL_GS=ON`); CPU é referência
  e fallback.
- [x] **Concluído (shadow executado):** religar com `DRA2_ENABLE_PARALLEL_GS=ON`
  e rodar `gpu-shadow` por 60/120 s sem crash, com progressão de apresentação
  equivalente ao CPU nos logs. O sink foi serializado com mutex próprio
  (`onLatch` roda sem `m_stateMutex`) e a cauda FIFO passou ao oráculo CPU.
- [ ] **Em progresso:** conectar reset, uploads, transferências local/local e
  local/host, CLUT, `TEXFLUSH`, ordenação GIF, interrupções e sincronizações
  GS usadas pelo jogo. No shadow, GIF/registradores/uploads/clear/escrita
  direta/reset/latch já chegam ao candidato; CLUT, `TEXFLUSH` e local/local
  são resolvidos dentro dele. Na captura longa só há host→local (203×) e
  `FINISH` (~1/latch); local→host/local→local e `SIGNAL`/`TEXFLUSH` não
  foram exercitados ao vivo.
- [ ] Definir quem possui cada região da VRAM, como alterações ficam visíveis
  e quando uma leitura do EE exige aguardar a GPU. Validar alias entre textura
  e framebuffer e leituras após escrita.
- [ ] Apresentar o framebuffer diretamente da GPU. Evitar readback completo e
  reupload por quadro, submissão por pixel/primitiva e espera global por desenho.
  Agrupar comandos respeitando dependências e limites de sincronização.
- [ ] Conectar a apresentação à janela existente, com tratamento explícito de
  contexto/dispositivo e threads; evitar cópias intermediárias desnecessárias.

**Entrega:** abertura e menu navegável acelerados, com speedup sustentado no
jogo, imagens de referência e caminho CPU funcional. Contar o tempo de toda a
integração, não somente a execução de shaders.

### 4. Substituir o custo dominante do VU1 quando a medição exigir — Não iniciada

Esta frente pode vir antes da integração completa da GPU se a etapa 1 mostrar
que o VU é o limitador principal.

- [ ] Inventariar os microprogramas realmente executados com REA/Ghidra e
  observação do runtime; identificar por conteúdo e versão dos overlays.
- [ ] Capturar entradas/saídas e comandos GIF dos programas mais caros.
- [ ] Traduzir esses programas para rotinas nativas C++/SIMD ou avaliar um
  recompilador VU existente, conforme cobertura e esforço de integração.
- [ ] Despachar por identidade exata do microprograma, com fallback ao
  interpretador para programas desconhecidos ou modificados.
- [ ] Comparar registros, memória VU e GIF emitido. Preservar efeitos relevantes
  de pipelines, branches, arredondamento, flags, `XGKICK` e sincronização VIF.

**Entrega:** redução mensurável do custo VU nas cenas reais, mantendo a ordem dos
comandos e os resultados. A troca por código nativo não pode apenas descartar
precisão ou trabalho que o jogo usa.

### 5. Validar partida e fechar o orçamento de quadro — Não iniciada

- [ ] Entrar numa partida, jogar e retornar ao menu; testar cenários com mais
  personagens/efeitos. Comparar imagem, controles, áudio e duração da simulação.
- [ ] Repetir medições sem debugger, registrando quadro mediano/p95, velocidade
  da simulação, hardware e configurações. Separar carregamento e aquecimento.
- [ ] Se EE/dispatcher ou IOP dominar após GS/VU, criar outra etapa dirigida a
  esse custo: HLE de rotinas conhecidas, despacho ou recompilação, com evidência.
- [ ] Aplicar PGO/LTO, especialização e ajustes menores somente quando ajudarem
  a fechar o orçamento e após confirmar que não mascaram erro de sincronização.

**Aceite:** velocidade de simulação correspondente ao original e cadência-alvo
sustentada nos cenários registrados, sem acelerar lógica, descartar desenho
necessário ou perder entrada/áudio. Informar limitações e regressões restantes.

## Regras para executar o roadmap

Cada etapa deve produzir uma alteração revisável, um relatório antes/depois e
uma decisão explícita sobre a próxima etapa. Dividir integração em commits
pequenos e manter correções genéricas nos patches locais do PS2Recomp.
Não trocar de backend e alterar timing do EE/VU no mesmo experimento.
O rasterizador CPU atual continua como referência; otimizações locais nele
ficam subordinadas às medições, sem consumir a prioridade da migração estrutural.

O próximo trabalho é **fechar o aceite das etapas 1 e 2** (aprovar as diferenças
visuais, completar cenários e métricas de apresentação) e avançar a **etapa 3**
da apresentação shadow para a apresentação GPU na janela. As etapas 4 e 5
ainda não foram implementadas.

## Referências para a avaliação

- [paraLLEl-GS: arquitetura e compromissos de Vulkan compute, pelo autor](https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation/)
- [LRPS2: integração e requisitos do paraLLEl-GS](https://github.com/libretro/docs/blob/master/docs/library/lrps2.md)
- [PCSX2: implementação e seleção dos renderizadores GS](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GS.cpp)
- [PS2Recomp: runtime utilizado pelo projeto](https://github.com/ran-j/PS2Recomp)

## Inventário da implementação entregue

- `patches/ps2recomp/0015-ps2xRuntime-performance-diagnostics-and-GS-capture.patch`:
  instrumentação, modo sem rasterização, captura GS e captura/replay VU no runtime.
- `tools/measure_performance.py`: matriz de medições sem debugger.
- `tools/capture_gs.py`: captura GS/VU, roteiro de entrada, metadados e corte
  no último checkpoint completo, preservando o arquivo bruto.
- `tools/gs_replay/`: replayers CPU/Vulkan e especificação do formato.
- `tools/build_gs_replay.sh`: build do candidato com revisão e dependências pinadas.
- `tools/benchmark_gs_replay.py`: pares alternados, imagens, tempos e evidências.
- `tools/test_gs_capture.py`: 11 testes de integração/validação em CPU e Vulkan.
- `tools/compare_gs_checkpoints.py`: métricas RGB e imagens de diferença por checkpoint.
- `tools/vu_replay.cpp` e alvo `dra2-vu-replay`: verificação e medição isolada VU.
- `runtime/parallel_gs/`: adaptador shadow no jogo (etapa 3), com mutex
  próprio, cauda FIFO via oráculo CPU e estatísticas a cada 300 latches.
- `docs/gs-acelerado.md`: comandos, fronteiras, custos e limites do protótipo.
- `progression/2026-11-07-gs-vulkan-replay-e-teto.md`: resultados, decisões,
  tentativas descartadas e pendências desta rodada.

**Build e estado de entrega:** runtime e executáveis diagnósticos compilados;
`dra2` religado usando a biblioteca gerada existente. Não foi feito rebuild
completo das cerca de duas mil ações antigas pendentes do código gerado.
A infraestrutura de captura/diagnóstico e a prova paraLLEl-GS são versionadas
em commits separados. Capturas, imagens, VRAM, snapshots VU e evidências REA
continuam em `.cache/`, fora do git.

## Scanout: descoberta posterior de 08/11

A exportação `--scanout-ntsc` revelou que SMODE1 está ausente da captura e do
estado preenchido pelo HLE. O replay histórico podia retornar imagem vazia
com `Unknown video format`: os 37,25× não comprovam custo de scanout válido.
A exportação diagnóstica agora produz imagem e metadados sob hipótese NTSC
explícita, com teste integral de pixels. Próximo passo da integração: preencher
o modo de vídeo no HLE e preservá-lo no shadow/captura antes de conectar a janela.
[Análise e validação](progression/2026-11-08-parallelgs-scanout-diagnostico.md).

## CRT corrigido e captura V3 (08/11)

O estado SMODE1 agora é preenchido pelo HLE SetGsCrt para NTSC/PAL, inclusive
pelo caminho `sceGsResetGraph` que o jogo usa. A captura nova `DRA2GS03`
preserva o registrador, o shadow o copia e `--scanout` exporta a imagem sem
hipótese NTSC. O teste real confirmou a primeira imagem GPU 640×448 após a
configuração CRT. Compatibilidade de leitura V2 preservada. Detalhes e limites:
[CRT e captura V3](progression/2026-11-08-parallelgs-crt-e-captura-v3.md).

A pendência de SMODE1 registrada acima foi resolvida para os modos TV cobertos.
A próxima etapa segue sendo validar a imagem e conectar a apresentação à
janela, com dispositivo/contexto e threads explícitos. Medir novamente scanout
válido e apresentação antes de atribuir ganho de FPS ao jogo.

## Primeira divergência isolada (08/11)

O primeiro comando divergente da captura V3 anterior foi isolado no registro
32. O patch 0021 preserva atributos constantes do triângulo na CPU, inclusive
sob FE_TOWARDZERO; esse prefixo agora é idêntico nos 4 MiB de VRAM CPU/GPU.
A redução também revelou escrita indevida de profundidade com ZTE=0, corrigida
separadamente pelo patch 0020. Essa segunda falha não era a causa do registro 32.

Uma captura nova passa nas seis referências CPU; GPU ainda diverge nas seis.
Próximo passo: isolar a primeira diferença restante nessa captura. As etapas
de aceite visual e apresentação continuam em progresso. Evidências, testes,
limites e artefatos: [primeira divergência corrigida](progression/2026-11-08-parallelgs-primeira-divergencia-corrigida.md).
