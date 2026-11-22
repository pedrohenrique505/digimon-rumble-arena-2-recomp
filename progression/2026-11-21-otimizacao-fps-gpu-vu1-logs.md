# Investigação e ganhos de FPS na cena 3D (GPU readback, VU1 e logs agressivos)

Sessão dedicada a diagnosticar e atacar as causas do FPS injogável no modo
`DRA2_GS_BACKEND=gpu-present`, usando o roteiro de controle
`tools/scenarios/menu-controle-rapido.txt`, amostragem de pilhas com REA/GDB e
inspeção estática com Ghidra MCP e `readelf`.

## Identidade (Ghidra MCP)

- `SLUS_210.67` (`r5900:LE:32:default`, entry `0x100008`,
  SHA-256 `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`).
- Confirmadas `FUN_003a2460` (envio DMA, troca de buffers, incremento do
  contador em `$gp+0x440` = `0x538930`, espera pelo flag `$gp+0x434` com timeout
  de 1 s via `FUN_003e5f50`) e `FUN_003fde64` (`strcmp` libc EE).
- `.vutext` (`0x004103e0`, 15.936 bytes = 1.992 pares de instruções): 40% das
  instruções upper são `NOP`, 34% das lower são `MOVE`, e **0 instruções** leem
  flags FMAC (`FM*`/`FS*`; apenas `FCAND`/`FCOR`/`FCSET` de `CLIP`).

## Causas-raiz comprovadas com REA/GDB

1. **Erro de thread index do Granite (`dra2_parallel_gs.cpp`)**:
   `Util::register_thread_index(0)` só era chamado na thread que inicializava o
   `Vulkan::Device`, gerando 945–3.902 mensagens `[ERROR]: Thread does not exist
   in thread manager or is not the main thread.` por execução na `GameThread`.
2. **Contenção de mutex e readback síncrono no `onLatch()`**:
   `ShadowSink::onLatch()` mantinha `m_mutex` travado durante `m_device.wait_idle()`
   e durante o downsample escalar 2560×448→640×448 + loop de alfa, bloqueando a
   `GameThread` por ~1,08 s a cada 5 s (~21% do tempo de parede).
3. **`VU1Interpreter` (83,3% do tempo da cena 3D de ~5,1 FPS)**:
   - 99,6% dos ciclos VU1 executados (`55,8M / 56,0M`, `budgetHits=0`) vêm de um
     único ponto de entrada (`pc=0x50`, `gen=25`), consumindo ~750 mil ciclos/quadro.
   - Custos dominantes comprovados nas amostras: `commitReadyPipelines()` varrendo
     6 arrays vazios a cada instrução (20%), `calculatePairReadyCycle()` varrendo
     registradores `VI` vazios (13%), `calculateFmacExactResult()` recomputando
     tudo em `long double` x87 (15%+), `memset` de 64 KB no `XGKICK`, 14 chamadas
     a `normalizeOperand()` mesmo em `NOP`, filas de escrita de `VF`/`ACC`/`VI`/`SQ`
     e lookup TLS por instrução.
4. **`PS2_FUNCTION_LOG_TRACKER` (`PS2X_ENABLE_AGRESSIVE_LOGS=ON`, 16,7%→26,7% do tempo)**:
   Cada função recompilada fazia `write()` + `flush()` em `game/ps2_log.txt`,
   gravando **138 MB em 35 s** (~4 MB/s de syscalls síncronas).

## Evolução medida no mesmo roteiro (`menu-controle-rapido.txt`, 35 s, sem `PS2X_PERF`)

| Etapa | Commit | Boot `[0–5 s]` | Abertura `[15–20 s]` | Cena 3D alvo (antes 5 FPS) | Observações |
| --- | --- | ---: | ---: | ---: | --- |
| Baseline (`gpu-present`) | `ca983b5` | 10,05 FPS | 21,66 FPS | **5,09–5,28 FPS** (`[20–35 s]`) | 945 erros Granite; 138 MB em `ps2_log.txt` |
| + Registro thread Granite | `e86a8c9` | 10,47 FPS | 21,75 FPS | 5,19–5,30 FPS (`[20–35 s]`) | **0 erros Granite** |
| + Readback Vulkan assíncrono + 4× rápido | `46f16b8` | — | **62,74 FPS** *(com `PS2X_PERF`)* | — | Stall de `gs` caiu de 1,08 s → **0,06 s** / 5 s; `present` CPU caiu 2,5×–4× |
| + VU1 bitmasks, cache ref, `double`, sem `memset` 64 KB | `512b91d` | 10,40 FPS | 43,07 FPS | **8,03–8,25 FPS** (`[20–35 s]`) | Patch 0027 (+60% na cena 3D) |
| + VU1 escrita direta, fast-path `NOP`, elisão de flags FMAC | `4ee8aef` | 17,84 FPS | 48,47 FPS | **14,39–14,70 FPS** (`[20–35 s]`) | Patch 0028 (~2,9× o baseline) |
| + VU1 sem TLS no loop quente, `NOP` upper/lower inline | `587ba69` | 17,81 FPS | **58,56 FPS** | **18,70–18,84 FPS** (`[20–35 s]`) | Patch 0029 (~3,7× o baseline) |
| + Silenciamento `ps2_log.txt` + `AGRESSIVE_LOGS=OFF` | `d431180` | **60,08 FPS** | — | **26,13–26,19 FPS** (`[10–20 s]`) | `ps2_log.txt` não criado (~5,15× o baseline); **0 erros `Unexpected Z format`**; boot 3,4× mais rápido fez o roteiro avançar até uma cena 3D ainda mais pesada em `[25–35 s]` (~11,5 FPS) |
| ~~+ VU1 SSE inline (`0030`)~~ *(revertido em `f6e3d20`)* | `92397d8` → `f6e3d20` | — | — | *(descartado)* | Introduziu corrupção visual no menu e 15–26 `[ERROR]: Unexpected Z format` (SSE `_mm_min_ps`/`_mm_max_ps` sem normalização de entrada `Inf`/`NaN` e `MXCSR` compartilhado); revertido integralmente. |
| + **Etapa 4: VU1 AOT (`tools/vu1_transpile.py` + patch `0030`)** | `d0c83ce`, `297e887`, `d47b680` | **61,89 FPS** | **121–125 FPS** (`[15–25 s]`) | **59,97 FPS** (`[30–35 s]`, menu carrossel 3D) | VU1 caiu de **~90% → 1,7%** da CPU (`0,151 s` em `9,02 s`); `dra2-vu-replay` **100% bit-a-bit** (16 snapshots + 5 casos de clipping do menu); **0** `Unexpected Z format`, **0** erros Granite, **~28% de folga em VBlank** a 60 FPS no menu. |

## Documentação do fluxo

O fluxo iterativo de perfilagem e otimização foi registrado em
[docs/fluxo-otimizacao-fps.md](../docs/fluxo-otimizacao-fps.md) e referenciado
em [docs/desempenho.md](../docs/desempenho.md).

## Próximos passos

1. **Testar cenas de batalha 3D** (4 Digimons + arena + efeitos) para verificar
   se há novos hashes de microprograma VU1 (`.DVP.overlay*`) a incluir no
   `tools/vu1_transpile.py` e medir se os 60 FPS se mantêm em combate.
2. **Reduzir overhead do emulador IOP (`advanceIopEeCycles`)**: no menu a
   59,97 FPS (`[25–35 s]`), `iop` ainda consome **31,4%** do tempo (`2,835 s` de
   CPU em `9,02 s`, com **1,28 milhão de chamadas**); agrupar/batching de ciclos
   IOP em `EeScheduler::accountCycles()` pode liberar ~25–30% de CPU extra para
   arenas mais pesadas.
3. **Rebuild limpo de `dra2_generated` com `PS2X_ENABLE_AGRESSIVE_LOGS=OFF`**
   quando for conveniente (elimina até o resíduo inline de `log_stream()`).
