# Fluxo iterativo de investigação e otimização de FPS

Este guia documenta o ciclo prático utilizado para investigar gargalos reais de
desempenho no `dra2`, combinando o roteiro de controle reproduzível, amostragem
de pilhas via REA/GDB, análise estática no Ghidra MCP e commits incrementais.

## Princípios

1. **Nunca adivinhar o gargalo**: toda otimização parte de uma amostra de
   pilhas ou contador exclusivo na cena que realmente está lenta.
2. **Medir a cena certa sem distorcer o timing**: o roteiro em
   [tools/scenarios/README.md](../tools/scenarios/README.md) usa tempo de parede.
   Probes pesados (como `PS2X_PERF`, que chama `clock_gettime` centenas de
   milhares de vezes por segundo no `IOP`) atrasam o boot e podem fazer o
   roteiro perder o aperto de botão e nunca entrar na cena lenta.
3. **Um gargalo por vez, em commits pequenos**: otimizar uma causa dominante,
   remedir no mesmo roteiro e só então coletar um novo perfil — porque o
   gargalo dominante muda a cada rodada.
4. **Iterar em segundos sem recompilar ~78k funções**: evitar alterar headers
   incluídos pela PCH de `dra2_generated` (como `ps2_vu1.h` ou `ps2_runtime.h`)
   enquanto se itera no runtime. Recompilar só `ps2_runtime` + religar `dra2`
   leva ~5–10 s em vez de ~35 min.

## Passo a passo

### 1. Estabelecer baseline no roteiro de controle (sem `PS2X_PERF`)

Use apenas `PS2X_PERF_FRAMES` (que grava uma linha por quadro em `FUN_003a2460`
com custo desprezível) e o roteiro `menu-controle-rapido.txt`:

```bash
mkdir -p .cache/performance/baseline
cd game
DRA2_ISO='/caminho/para/Digimon Rumble Arena 2 (USA).iso' \
  DRA2_GS_BACKEND=gpu-present \
  DRA2_PAD_REPLAY="$PWD/../tools/scenarios/menu-controle-rapido.txt" \
  PS2X_PERF_COUNTER=0x538930 \
  PS2X_PERF_FRAME_PC=0x3a2460 \
  PS2X_PERF_FRAMES="$PWD/../.cache/performance/baseline/frames.jsonl" \
  timeout -s INT 35 ../build/dra2 "$PWD/SLUS_210.67" \
  > ../.cache/performance/baseline/run.log 2>&1
```

Agrupe `frames.jsonl` em janelas de 5 s (`fps` e `ee_speed = Δee_cycle / 294912000 / Δt`)
para localizar exatamente em que intervalo de tempo o FPS desaba (ex.: `t=20–35 s`).
Observe também o contador de latches/pacotes GIF no `run.log` (`[dra2-gs] shadow latches=... gif=...`):
quando o jogo fica mais rápido, o roteiro por tempo de parede pode avançar para
cenas mais pesadas na janela final — compare sempre cenas equivalentes.

### 2. Amostrar pilhas na janela lenta com REA/GDB

No Linux com `kernel.yama.ptrace_scope = 1`, o GDB não anexa a processos irmãos;
inicie o `dra2` sob o GDB (via REA `start_gdb_session` + `gdb_console`) com um
script que:

1. Desliga sinais não fatais (`handle SIGPIPE SIGUSR1 SIGUSR2 SIGALRM nostop noprint pass`).
2. Captura o ponteiro da RDRAM (`$rdi`) em um breakpoint one-shot em `FUN_003a2460_0x3a2460`
   (desabilitado na primeira parada, custo zero depois).
3. Deixa o jogo rodar **livre** durante o aquecimento (ex.: 22 s) até entrar na
   cena lenta, sem interromper o processo durante o roteiro de controle.
4. Coleta ~30 amostras em intervalos de 0,35–0,4 s (`SIGINT` via `threading.Timer`),
   registrando a pilha completa de `GameThread` e da thread principal (`dra2`),
   junto do contador `0x538930`.
5. Fecha a sessão com `close_gdb_session` ao terminar.

Analise **leaf** (função no topo da pilha — custo próprio) e **inclusive**
(funções presentes na pilha — subsistema dominante) separadamente para
`GameThread` e para a thread de apresentação.

### 3. Cruzar com Ghidra MCP e inspeção estática do ELF

Com os endereços quentes em mãos:

- Use o **Ghidra MCP** (`decompile_function`, `decompile_function_by_address`)
  para confirmar o papel das rotinas do jogo (`FUN_003a2460` no envio/troca de
  buffers, `FUN_003e5f50` no limitador de quadro, rotinas de cena como
  `FUN_002f9448` e `FUN_0023c460`).
- Para gargalos de VU1, inspecione `.vutext` e `.DVP.overlay*` no ELF
  (`readelf -S game/SLUS_210.67`) e meça ciclos/PCs de entrada em runtime.
  Isso revela propriedades estruturais do microcódigo (por exemplo: 99,6% dos
  ciclos em um único `pc=0x50`, 40% de `NOP` upper, ausência de leitura de flags
  `FM*`/`FS*`) sem precisar de heurísticas cegas.
- Antes de propor solução para comportamento de hardware PS2 (VU1, GS, DMA,
  arredondamento), consulte referências primárias (PCSX2, PS2Tek, ps2sdk).

### 4. Implementar e compilar rápido (ciclo de ~5–10 s)

- Mudanças em código nosso (`runtime/parallel_gs/`, `runtime/overrides/`):
  `ninja -C build dra2 -j5`.
- Mudanças em `.cpp` do runtime (`external/PS2Recomp/ps2xRuntime/src/...`):
  **não altere headers da PCH** durante a iteração; compile apenas `ps2_runtime`
  e relink o `dra2` contra a `libdra2_generated.a` existente:

  ```bash
  ninja -C build ps2_runtime -j5
  (cd build && eval "$(ninja -t commands dra2 | tail -n 1)")
  ```

- Se precisar tocar em algum header do runtime, faça um build limpo de
  `ps2_runtime` (`ninja -C build -t clean ps2_runtime`) para evitar
  descompasso de ABI/tamanho de structs entre objetos.
- Para mudanças em `external/PS2Recomp/`, extraia um patch isolado em
  `patches/ps2recomp/NNNN-descricao.patch` (apenas o delta sobre os patches
  anteriores) e verifique com `git -C external/PS2Recomp apply --reverse --check -C0`.

### 5. Remedir, registrar e repetir

1. Execute o mesmo roteiro de 35 s e compare janela a janela (FPS, velocidade
   EE, contadores `[dra2-gs]`, ausência de erros no log e integridade de
   `game/mc0`).
2. Faça um commit pequeno e descritivo por melhoria confirmada (sempre revisando
   `git status` para não versionar nada de `game/`, `generated/` ou `.cache/`).
3. Colete um novo perfil REA/GDB para confirmar que o gargalo atacado encolheu
   e identificar o novo topo da pilha.

## Armadilhas conhecidas

- **Overhead do `PS2X_PERF`**: `CLOCK_THREAD_CPUTIME_ID` é syscall no Linux; em
  escopos chamados ~96k vezes/s (como `advanceIopEeCycles`), distorce o tempo
  real. Use `PS2X_PERF` apenas para breakdown relativo entre subsistemas e
  `PS2X_PERF_FRAMES` sozinho para FPS real.
- **Logs agressivos (`PS2X_ENABLE_AGRESSIVE_LOGS=ON`)**: injeta `PS_LOG_ENTRY`
  em cada função recompilada, gravando e fazendo `flush()` de >100 MB em
  `game/ps2_log.txt`. Desligue por padrão no `CMakeLists.txt`; em builds com
  `libdra2_generated.a` já compilada, sobrescreva apenas `ps2_log::log_stream()`
  (não `log_entry`/`log_exit`, para não desbalancear `ps2_log::depth()` em
  chamadas inline).
- **Thread index do Granite (`paraLLEl-GS`)**: qualquer thread que chame
  `Vulkan::Device`/`GSInterface` precisa chamar `Util::register_thread_index(0)`
  (sob mutex de serialização); sem isso, o Granite gera milhares de `[ERROR]`
  no `stderr` por execução.
- **Contenção de mutex no `onLatch()`**: `m_device.wait_idle()` e downsample
  CPU sob o mesmo mutex de `onGifPacket()` travam a `GameThread`. Submeta a
  cópia com `Vulkan::Fence` em ring buffer e faça espera/downsample fora do
  mutex.
