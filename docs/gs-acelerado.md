# Prova de GS acelerado e diagnóstico de desempenho

Esta implementação atende à medição e ao replay das etapas 1 e 2 do
[NEXTSTEPS](../NEXTSTEPS.md). O jogo continua usando o GS CPU. O executável
Vulkan é uma prova offline: não conecta a GPU ao EE/IOP nem à janela do jogo.

## Medir sem debugger

```bash
export DRA2_ISO='/caminho/para/Digimon Rumble Arena 2 (USA).iso'
python3 tools/measure_performance.py .cache/performance/minha-matriz
```

O padrão faz cinco execuções de cada variante: baseline, tracing diagnóstico,
sem rasterização e sem contadores de subsistema. A ordem se inverte a cada
rodada. O binário, otimização e memory card são os mesmos; hashes e configuração
ficam em `metadata.json`. Cada processo mede os mesmos quadros 200–260 do contador do jogo,
na tela de aviso de autoload, sem entrada. Os quadros iniciais ficam fora da
janela. Isso evita comparar fases diferentes quando uma variante avança mais
rápido pelo carregamento e pelas transições. Não compilar nem executar outro
benchmark durante a coleta. `--compare raster|trace|probe` permite repetir só
uma comparação. `--frame-start` e `--frame-end` controlam a janela lógica; `--timeout` limita
a duração do processo. A aquisição inicial usou processos de 33 s e foi
reanalisada pelos mesmos quadros, preservando os resumos exploratórios por tempo
de parede.

O Ghidra confirmou `FUN_003a2460`, que envia os buffers e incrementa
`$gp+0x440`, endereço `0x00538930` neste ELF. Os hooks abrangem chamadas diretas
e retomadas pelo scheduler. O script exige correspondência um a um entre hooks
e contador. O intervalo mediano/p95 é de **submissão do jogo**, não de imagens
únicas mostradas. `window_swaps` conta iterações de apresentação da janela e
inclui repetições da mesma imagem. A velocidade de simulação vem do relógio EE
(294.912.000 ciclos/s); ela não é inferida do FPS da janela.

Os relógios de CPU da thread e de parede usam a mesma ordem nas duas bordas de
cada scope. Custos filhos são subtraídos dos pais na mesma thread: GS chamado
por XGKICK não pode ser somado novamente como VU inclusivo. A janela de custos
usa relatórios de aproximadamente 1 s dentro da janela de quadros; sua duração
exata é registrada. Os contadores incluem seu overhead, principalmente em
chamadas muito curtas do IOP. A variante `no-probe` quantifica a perturbação.
A diferença parede menos CPU é uma estimativa de espera, sujeita às bordas dos
scopes e ao custo de ler os relógios; não tratar diferenças pequenas como trabalho
negativo. A comparação de tracing alterna `DRA2_TRACE_GS/RPC`, mantendo os logs
compilados do runtime. Ela não equivale a um build sem `AGRESSIVE_LOGS`.

`PS2X_NO_RASTER=1` omite primitivas e clears por pixel. Continua consumindo GIF,
carregando CLUT, transferindo memória e executando registros/sinais. A imagem
resultante é inválida e leituras de regiões que seriam desenhadas podem alterar
o comportamento. Seu resultado é somente um teto diagnóstico; não é FPS jogável.
A captura registra consumos local/host e escritas diretas; sua ausência num fluxo
não prova ausência de feedback entre textura e framebuffer em outras cenas.

## Capturar e reproduzir GS

```bash
python3 tools/capture_gs.py .cache/performance/captura --seconds 35
bash tools/build_gs_replay.sh
.cache/gs-replay-build/dra2-gs-replay \
  .cache/performance/captura/complete.gs .cache/performance/cpu
.cache/gs-replay-build/dra2-gs-replay-vulkan \
  .cache/performance/captura/complete.gs .cache/performance/gpu
python3 tools/test_gs_capture.py .cache/gs-replay-build/dra2-gs-replay
python3 tools/benchmark_gs_replay.py .cache/performance/captura/complete.gs \
  .cache/performance/replays --build .cache/gs-replay-build
```

Para avançar pela abertura, `capture_gs.py --inputs roteiro.json --seconds 90`
aceita uma lista como `[{"seconds":10,"key":"x","hold":0.7}]`. As teclas permitidas
são x/z, Return e setas. O script encontra uma única janela visível do PID que
acabou de iniciar, foca essa janela e solta cada tecla em `finally`. Horários
solicitados e reais são preservados. O roteiro não demonstra que cada cena
esperada foi alcançada: conferir as imagens e o estado antes de nomear o cenário.

A aquisição muda o timing e salva dados do jogo. Ela nunca é uma medição de
FPS. O processo é encerrado e a cauda posterior à última referência completa de
VRAM é descartada numa **cópia**, com tamanho registrado. O bruto é preservado.
Os replayers recusam registros truncados; o CPU também retorna erro se qualquer
referência divergir. Os resultados exportam `.vram`, `.ppm` e JSON no stdout.
Escolher um prefixo novo em cada execução.

O formato e seus limites estão em [tools/gs_replay/FORMAT.md](../tools/gs_replay/FORMAT.md).
A versão 2 registra o modo de arredondamento de ponto flutuante do host. Isso é
necessário porque VU1 chama GS com FE_TOWARDZERO ativo. A primeira captura sem
esse campo falhou no próprio CPU e foi descartada da comparação. Não foi
alterada a aritmética do VU ou a imagem do rasterizador para fazer o teste passar.

## Replay VU isolado

```bash
cmake --build build --target dra2-vu-replay -j3
build/dra2-vu-replay .cache/performance/captura/vu-0.vu
```

`capture_gs.py` salva as primeiras 16 fatias VU1 que emitem GIF. Cada arquivo
inclui registradores, pipelines, código/dados e o XGKICK pendente. O replay
verifica registradores, memória e tamanho/hash dos comandos GIF emitidos,
usando um sink que não desenha. O cache de instruções é preparado antes das
medições, preservando o caminho de cache usado ao vivo. Uma execução aquece e
cinco são medidas. Os snapshots são específicos da mesma revisão/build nativa;
não são um formato público ou uma entrada não confiável. Essa amostra inicial
não identifica automaticamente os microprogramas mais caros da partida.

## Candidato e fronteira de integração

| Questão | Avaliação |
| --- | --- |
| Candidato | paraLLEl-GS, revisão `cc6184af7e0c03da603045ca371ffa5dae9b0655` |
| Dependência principal | Granite `16e7395f6a4858c1783dbf6f521f90b9d5f82ac5`; Volk, Vulkan headers e bibliotecas auxiliares pinadas pelos submódulos |
| Build | CMake/Ninja, C/C++, `PARALLEL_GS_STANDALONE=ON`; não precisa do frontend SDL para este replay |
| Entrada | `GSInterface::gif_transfer`, registradores e uploads; captura após arbitragem GIF e antes do rasterizador |
| VRAM | `map_vram_write/end_vram_write` para a semente e `map_vram_read` para conferir referências |
| Scanout | `GSInterface::vsync`; a saída é uma imagem Vulkan, sem swap de janela neste protótipo |
| Licença declarada | LGPL-3.0-or-later, conforme README e cabeçalhos do candidato; código externo baixado em `.cache/`, não vendorizado |
| Hardware testado | Radeon Renoir integrada, RADV/Mesa 26.2.5, Vulkan 1.4.354; inicialização e replay compute executados |

O upstream lista descriptor indexing, timeline semaphores, storage de 8/16 bits,
shaderInt16, scalar block layout e suporte a subgroups. O replay executado na
GPU local é a verificação prática usada aqui. `build_gs_replay.sh` busca apenas
os submódulos necessários: o clone recursivo completo trouxe dados de testes
ASTC opcionais e excedeu a quota de `/tmp`. Fontes/builds novos devem ficar no
`.cache/` do disco, não no tmpfs pequeno.

`GSRasterBackend::Submit` já recebe primitivas decodificadas, portanto não é a
interface correta para encaixar diretamente um consumidor de GIF. Este teste
mantém o frontend CPU para a referência e alimenta o candidato na fronteira
anterior. O clear HLE tem fallback CPU com ida/volta de VRAM medida. O candidato recusa consumo local/host, cuja semântica de bytes parciais ainda
precisa de adaptador. Escritas diretas usam o swizzle CPU sobre VRAM mapeada
com sincronização e devolução das páginas ao rastreador GPU.

A alternativa PCSX2 continua disponível, mas seu `GS.cpp` envolve o estado GS,
criação de dispositivos/renderizadores, configuração e integração com Host. A
estimativa é de uma extração com mais acoplamentos locais do que a API standalone
já exercitada do paraLLEl-GS. Não foi construída nem medida uma extração PCSX2.
O custo local atual é um adaptador de replay e versões pinadas; integração ao
jogo ainda exigirá coerência de VRAM, SIGNAL/FINISH, readback e apresentação.

Fontes primárias:
[paraLLEl-GS](https://github.com/Arntzen-software/parallel-gs),
[API da revisão testada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_interface.hpp),
[arquitetura pelo autor](https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation/),
[fronteira GS do PCSX2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GS.cpp).

## Comparação por checkpoint e coerência de escrita

O replay aceita `--checkpoints` como terceiro argumento. Ele cria uma pasta
`<prefixo>.checkpoints` nova, com PPM e posição de evento/latch para cada
referência. A exportação fica em `validation_seconds`; não usar esta execução
para substituir os cinco pares do benchmark.

```bash
.cache/gs-replay-build/dra2-gs-replay captura.gs .cache/cpu --checkpoints
.cache/gs-replay-build/dra2-gs-replay-vulkan captura.gs .cache/gpu --checkpoints
python3 tools/compare_gs_checkpoints.py .cache/cpu.checkpoints \
  .cache/gpu.checkpoints .cache/comparacao
python3 tools/test_gs_capture.py .cache/gs-replay-build/dra2-gs-replay-vulkan --gpu
```

O comparador exige os mesmos checkpoints/eventos/latches e referência CPU exata.
Exporta diferenças RGB e métricas por checkpoint. Mede o DISPFB bruto de 640×448;
não mede a imagem de `vsync`, deinterlace ou apresentação na janela e não concede
aprovação visual automática. Use sempre a mesma captura nos dois replays.

Na revisão pinada, `GSInterface::init_transfer` calcula o tamanho host/local
com divisão inteira por 64. O teste de upload de um pixel CT32 perde a escrita;
um upload CT24 com três pixels também necessita tratar o último word parcial.
O adaptador de **upload nativo** usa transferência CPU sobre VRAM sincronizada
quando o tamanho em bits não é múltiplo de 64. Isso preserva os registradores
GPU e contabiliza o custo em `work_seconds`; `native_upload_fallbacks` informa
quantas vezes ocorreu. O checkout externo permanece sem alterações. Esta
correção não cobre uploads IMAGE arbitrários recebidos no evento GIF tipo 1.
A leitura FIFO continua sendo recusada explicitamente.

A escrita direta (evento 9) força `flush`, mapeia os 4 MiB, aplica `WriteVram`
CPU e chama `end_vram_write`. É conservadora e síncrona; precisa ser refinada
antes de um caminho de uso frequente. Os testes incluem upload GPU real seguido
de escrita CT24 com preservação do alfa e checagem integral da VRAM.

Resultados e evidências desta continuação:
[diário de 08/11](../progression/2026-11-08-parallelgs-coerencia-e-checkpoints.md).

## Etapa 3: shadow no jogo

Build opcional (padrão desligado) e execução:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DDRA2_ENABLE_PARALLEL_GS=ON
cmake --build build --target dra2 -j5
cd game && DRA2_GS_BACKEND=gpu-shadow DRA2_ISO=/caminho/para/a.iso \
  timeout -s INT 60 ../build/dra2 "$PWD/SLUS_210.67"
```

O sink (`runtime/parallel_gs/`) espelha GIF pós-arbitragem, registradores
diretos, uploads nativos, consumo local→host, clear, escrita direta, reset
e latch no candidato. CLUT, `TEXFLUSH` e transferências local→local são
resolvidos dentro do candidato a partir desse fluxo. O CPU continua
autoritativo: VRAM, SIGNAL/FINISH, readback ao EE e apresentação (raylib)
não mudam. Os callbacks são serializados por mutex próprio, porque
`onLatch` é chamado sem o mutex de estado do frontend. A cada 300 latches
o stderr recebe `[dra2-gs] shadow latches=... gif=... nativeFallbacks=...
fifoReq=... fifoBytes=...`. Sem Vulkan, o sink se desliga com aviso e o
jogo segue no CPU. Apresentação GPU na janela e promoção a backend
autoritativo não fazem parte deste modo.
