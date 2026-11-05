# 2026-11-04: Do SIF até o laço de render

Build completo no sistema novo: 31 min na 1ª vez. Depois disso, uma mudança no override recompila em **3 a 4 s**, e uma no runtime em cerca de 20 s.

## Bloqueios resolvidos (em ordem)

### 1. `sceSifInitRpc`: resolvido pelos 98 bindings HLE do SDK
- Com os bindings, o SIF inicializa e **os 10 IRX do jogo carregam e rodam no IOP emulado** (`ps2xIOP`), incluindo o `IOPHOOEY.IRX`, que é o driver próprio do jogo (servidores "disk", "io", áudio e streaming).

### 2. `Couldn't bind io system rpc.`: o bind bloqueante não esperava o IOP
- `0x3EAC28` (init do IOP) faz **um único** `sceSifBindRpc(&client, 0x11, 0)` e dá erro fatal se `client.server == 0`.
- No console, o bind bloqueante espera a resposta do IOP, e enquanto isso a thread do IRX registra o servidor. O runtime retorna na hora.
- **Override** `sifBindRpcBlocking`: se o SID ainda não existe, deixa passar tempo de EE (`eeScheduler().accountCycles`, API pública que avança timers e o IOP) em fatias de 1 ms. Na prática esperou 1 ms.

### 3. `scePadPortOpen(0,1) failed`: multitap
- `0x3EDDF8` abre os 8 controles (port = i&1, slot = i>>1) e dá erro fatal se qualquer abertura falhar. O HLE de pad só tem o slot 0.
- **Override** `padPortOpenMultitap`: os slots 1 a 3 abrem com sucesso. O resto do HLE já reporta "desconectado" para eles.

### 4. Laço infinito no `sceSifCallRpc`: bug de convenção no bind por endereço
- Com `DRA2_TRACE_RPC=1`: 508 mil entradas em 20 s no mesmo `CallRpc` (client `0x542E10`, SID `0x10`, NOWAIT, com `end_function`), e nenhuma retorna.
- Causa: o HLE agenda o callback com uma **cópia do contexto** para retomar depois. Os stubs gerados pelo `ps2_recomp` fazem `ctx->pc = $ra` **antes** de chamar o handler, mas o `bindAddressHandler` do upstream não faz isso. Na retomada, o `pc` volta para a entrada da função, que roda de novo.
- **Correção:** trampolim próprio (`sdkTrampoline`) que faz `pc = ra` e depois chama o handler (os nomes são resolvidos pelas X-macros `PS2_SYSCALL_LIST`/`PS2_STUB_LIST`). Convenção documentada no topo do `dra2_overrides.cpp`.

### 5. IOP em loop no `cdvdman:14` (patch 0001)
- O `IOPHOOEY` consulta a bandeja do leitor em loop. O export 14 é o `sceCdTrayReq` ([ps2sdk exports.tab](https://github.com/ps2dev/ps2sdk/blob/master/iop/cdvd/cdvdman/src/exports.tab)). Semântica no [cdvdman.c do ps2sdk](https://github.com/ps2dev/ps2sdk/blob/master/iop/cdvd/cdvdman/src/cdvdman.c): `SCECdTrayCheck` escreve em `*traychk` e retorna 1.

### 6. `Directory record straddles sector boundry?`: bug de CPU do IOP (patch 0002)
- O `IOPHOOEY` tem um parser ISO9660 próprio. Ele leu a raiz no LSN **524557** em vez de **269**: `0x8010D` contra `0x10D`, um valor misturado.
- Causa: o par `lwl`/`lwr` (load desalinhado; o LSN da raiz fica no offset 158 do PVD). No R3000, a 2ª instrução do par enxerga o valor **pendente** do load delay slot da 1ª ([PSX Notes, unaligned loads](https://vxpm.github.io/psxnotes/cpu/unaligned_load_store.html)). O `ps2xIOP` fazia o merge com o valor antigo do registrador.
- Depois da correção, o driver percorre os diretórios certinho (269, 270, 279…).

### 7. Limitador de frame preso: COP0 Count parado (patch 0003)
- `0x3A2460` é o flip de fim de frame. Em `0x3A2800`, ele espera até `FUN_003E5F50` (tempo decorrido pelo Count do COP0, divisor `589824000.0`) passar de 1,0, enquanto a flag `gp+0x434` (frame no GS) estiver ligada.
- O `ctx->cop0_count` **nunca era atualizado** pelo runtime. Agora ele segue o `m_eeCycle` do scheduler (o mesmo relógio do VBlank) a cada `dispatchGuestBranch`.

### 8. Interrupção do GS (patch 0004)
- Quem zera a flag `0x434` é o handler de GS `0x3A4D48` (INTC causa 0), quando o `GS_CSR` tem FINISH. O GS do runtime liga o bit, mas ninguém gerava a interrupção. Agora o SIGNAL/FINISH pendente (respeitando IMR bits 8/9) é despachado no VBlankStart.

## Onde estamos
- O jogo roda o **laço principal**: lê o disco pelo próprio driver, registra o servidor RPC do EE (SID `0x20`) e manda DMA para o VIF1/GIF todo frame.
- **Bloqueio atual:** o FINISH **nunca chega ao GS**. Com `DRA2_TRACE_GS=1`, o `csr` mostra só o `0x2000` (FIELD). O handler de GS está registrado e habilitado, e o IMR é 0. Então cada frame espera o timeout de 1 s emulado.
- Hipótese: o pacote de fim de frame passa por VIF1 → **microcódigo do VU1** (`.vutext` e os 8 overlays DVP) → XGKICK → GIF, e esse pipeline não está entregando ao GS.
- Pendente: `[IOP] Couldn't bind callback rpc.`. O `IOPHOOEY` tenta fazer bind (do lado IOP) ao servidor que o EE registrou (SID `0x20`).

## Próximos passos
1. Seguir o DMA do VIF1 (`tadr=0x5FAD80`, tag END com `qwc=0xA2`): ver os VIFcodes (MPG/UNPACK/MSCAL/DIRECT), se o VU1 roda e se o XGKICK chega ao GIF. Pesquisar como o PS2Recomp/eagle_five tratam o VU1.
2. Investigar o "callback rpc" (bind IOP → EE).
3. Seguir corrigindo nós mesmos os problemas do runtime/IOP, como patches em `patches/ps2recomp/` (decisão de 2026-11-04: não abrimos issues/PRs no upstream).
