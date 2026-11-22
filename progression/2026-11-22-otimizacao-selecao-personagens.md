# Investigação e otimização de FPS na transição e na Seleção de Personagens (Issue #2)

Sessão dedicada a resolver a [issue #2](https://github.com/pedrohenrique505/digimon-rumble-arena-2-recomp/issues/2)
(engasgo na transição `Multiplayer → Seleção de Personagens` e queda para ~30–36 FPS na tela de Seleção de Personagens sob `DRA2_GS_BACKEND=gpu-present`),
seguindo o fluxo iterativo com **Ghidra MCP**, amostragem de pilhas via **REA/GDB** e commits pequenos validados a cada etapa.

Os valores de FPS abaixo medem latches. A comparação com o PCSX2 mostrou
que o runtime podia publicar 60 latches por segundo com a mesma imagem.
A correção e as medições de conteúdo ficaram no [diagnóstico do scanout](2026-11-22-scanout-selecao-personagens.md).

## Identidade e evidências estáticas (Ghidra MCP)

- Binário confirmado via `get_program_info`: `SLUS_210.67` (`r5900:LE:32:default`, entry `0x100008`, SHA-256 `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`, 13.890 funções).
- Decompilação das funções quentes observadas na pilha da `GameThread`:
  - `FUN_004154d8`: montador da display list VIF1/VU1 (emite `MSKPATH3(0)` = `0x06000000` e `FLUSHA`/`MSCNT` = `0x13000000`).
  - `FUN_002f9448`: rotina principal de renderização de cena.
  - `FUN_0023c460`: atualização/submissão de elementos da cena de menu/seleção.
- Análise estrutural do microcódigo VU1 (`vu1_code.bin`, hash `0xf091cd9ee4c393ca`): 1.981 pares não-nulos, apenas 2 `E-bit`s (`0x50`, `0x39a8`), 68 alvos diretos de branch, 20 `BAL`, 10 `JR` e 0 `JALR`. A emissão anterior de `case L_xxxx:` para todos os 1.981 pares fragmentava o CFG em 1.981 blocos básicos de 1 instrução, impedindo alocação de registradores XMM/GPR do GCC.

## Causas-raiz comprovadas com REA/GDB

Amostragens de pilha da `GameThread` com `start_gdb_session` + `gdb_console` na janela de transição e Seleção de Personagens (`tools/scenarios/selecao-personagens.txt`):

1. **VU1 AOT sem inline nos helpers quentes (`52%` da `GameThread` no baseline)**:
   - `nm -C vu1_recompiled.cpp.o` confirmou que `vuMadd`, `vuMsub`, `vuNorm`, `vuTickClip`, `vuTickQ`, `vuWaitQ` e a lambda `stallVi` **não foram inlined** pelo GCC (estouro do limite de `large-function-growth`), virando chamadas de função reais a cada instrução VU1.
   - `dra2Vu1NativeDispatch` repetia `std::fegetround`/`std::fesetround(FE_TOWARDZERO)` já executados em `VU1Interpreter::run()`.
   - `vuXgkick` copiava cada pacote quadword a quadword para um buffer de 64 KB na stack mesmo quando contíguo na VU1 data memory.
2. **Caminho CPU de GIF/GS redundante em `gpu-present` (`25%` + `12,5%` da `GameThread`)**:
   - `ShadowSink::onGifPacket` varria qword a qword com divisão inteira (`i % nreg`) pacotes PACKED sem nenhum registrador `A+D` (`0x0e`).
   - `PS2Memory::submitGifPacket` alocava `std::vector` + `memcpy` + `std::stable_sort` no `GifArbiter` mesmo com a fila vazia e `drainImmediately=true`.
   - `GS::processGIFPacket` / `GS::vertexKick` / `GS::buildDrawBatch` ainda decodificavam todos os vértices no frontend CPU mesmo com `skipCpuRaster() == true`.
   - `GS::processImageData` (`GSCpuBackend::UploadImage` → `GSMem::WriteP8`, **12,5% da `GameThread`**) ainda fazia swizzle e cópia de todas as texturas/retratos na VRAM CPU com `gpu-present` ativo (`fifoReq=0`).
3. **Overhead de `ps2_log::depth()` em `libdra2_generated.a` (`15,6%` da `GameThread`)**:
   - Mesmo com `log_stream()` redirecionado para `nullStream`, chamadas inline de `log_entry`/`log_exit` incrementavam `ps2_log::depth()` (~15–30 níveis), executando ~25 chamadas a `log_stream() << '\t'` + `flush()` a cada entrada/saída de função MIPS.
4. **Fragmentação de blocos básicos e `vuNorm()` redundantes em `vu1_prog_f091cd9ee4c393ca` (`43,8%` após limpar GS/log)**:
   - Normalização redundante de `acc` (já normalizado), `b` (broadcast já normalizado), `vf[0]` e registradores `vf` já escritos por FMAC no mesmo bloco básico, além de 1.981 `case`s no `dispatch:`.

## Commits aplicados (incrementais)

1. `dee4ff9` — `perf(vu1): força inline nos helpers quentes do AOT e fast-path zero-copy em vuXgkick`
2. `54ea7d6` — `perf(gs): evita varredura qword-a-qword em ShadowSink::onGifPacket sem A+D`
3. `1938c23` — `perf(gs): pula parse de vértices CPU em gpu-present e evita cópia no GifArbiter vazio (patch 0036)`
4. `6cf8de4` — `perf(gs): pula upload e clear na VRAM CPU quando gpu-present está ativo (patch 0037)`
5. `aa57a7f` — `perf(vu1): elimina vuNorm redundantes em acc/broadcast/vf0 e unifica vuTickPipes`
6. `86834e3` — `perf(ee): zera ps2_log::depth() e silencia log_entry/log_exit quando log está desligado`
7. `dc72bf8` — `perf(vu1): emite dispatch só para líderes de bloco básico e rastreia norm_vf estaticamente` (reduz de 1.981 para 241 líderes de bloco básico, incluindo entradas alternativas de shaders após preâmbulos como `0x0ba0` e `0x0e28`, validado bit-a-bit contra o interpretador).
8. `b0122c0` — `test(perf): adiciona roteiro de replay até Multiplayer -> Seleção de Personagens` (`tools/scenarios/selecao-personagens.txt`).

## Evolução medida no roteiro `tools/scenarios/selecao-personagens.txt` (32 s, sem `PS2X_PERF`)

| Etapa | Transição `[18–22 s]` | Seleção `[22–24 s]` | Seleção `[24–32 s]` | `max_dt` na Seleção | Quadros totais (32 s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Issue #2 (baseline `ae183f1`) | 38–54 FPS | ~29–36 FPS | ~29–36 FPS | 33–50 ms | — |
| Após commits 1–3 (`1938c23`) | 39,9–50,6 FPS | 46,03 FPS | 50,7–53,1 FPS | 26,0–34,3 ms | 1.660 |
| + Patch 0037 (`6cf8de4`) | 42,1–53,3 FPS | 51,95 FPS | 55,4–58,1 FPS | 28,8–32,9 ms | 1.690 |
| + VU1 v2 (`aa57a7f`) | 42,6–49,7 FPS | 49,99 FPS | 57,5–58,9 FPS | 22,7–33,7 ms | 1.690 |
| + `null_func_log` (`86834e3`) | 42,7–50,6 FPS | 43,59 FPS | 53,9–60,0 FPS | 18,4–27,1 ms | 1.685 |
| + **VU1 v3b (241 líderes + `norm_vf`)** | **58,9–60,0 FPS** | **60,95 FPS** | **59,96–60,07 FPS** | **19,5–22,0 ms** | **1.839** |

- **0** erros `Unexpected Z format`, **0** erros Granite, **0** `readbackFailures`, **0** misses do VU1 AOT.
- 993.742 pacotes GIF processados em 32 s (~930 pacotes/quadro na Seleção de Personagens) mantendo **60,00 FPS estáveis** (`ee_spd = 1.00x`).
