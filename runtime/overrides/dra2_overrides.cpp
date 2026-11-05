// Game overrides de Digimon Rumble Arena 2 (SLUS_210.67, NTSC-U v1.02).
//
// Código nosso, compilado no executável dra2 (CMakeLists.txt da raiz). Usa a API
// game_overrides.h do PS2Recomp, que só aplica estas correções quando o nome do
// ELF, o entry e o CRC32 batem.
//
// Convenção dos handlers: ctx->pc = $ra ANTES de chamar qualquer HLE do runtime,
// como fazem os stubs gerados pelo ps2_recomp. Handlers que guardam o contexto
// para retomar depois (ex.: sceSifCallRpc com end_function) usam esse pc; se ele
// ainda apontar para a entrada da função, a retomada a executa de novo, em laço.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_iop_transport.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "ps2_recompiled_functions.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace
{
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kElfCrc32 = 0xA6702034u;

    // 0x00408B68: write(fd, buf, len) da libkernel para fd 1/2 (stdout/stderr).
    // O original manda o texto pela TTY do DECI2 (0x004096E0): sceDeci2ReqSend e
    // depois um loop de sceDeci2Poll até um callback zerar a flag em 0x00569E5C.
    // Sem o kit de desenvolvimento esse callback nunca vem, e o jogo trava no
    // primeiro printf. Aqui o texto vai direto para o stdout do host.
    void ttyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t buf = getRegU32(ctx, 5);
        const int32_t len = static_cast<int32_t>(getRegU32(ctx, 6));

        const uint8_t *host = (fd == 1 || fd == 2) ? getConstMemPtr(rdram, buf) : nullptr;
        if (!host || len < 0)
        {
            setReturnS32(ctx, -1);
        }
        else
        {
            std::fwrite(host, 1, static_cast<size_t>(len), fd == 2 ? stderr : stdout);
            std::fflush(fd == 2 ? stderr : stdout);
            setReturnS32(ctx, len);
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    // 0x0040B168: sceSifBindRpc(client, sid, mode).
    // No console, o bind bloqueante (mode 0) só retorna quando o IOP responde, e
    // enquanto isso as threads do IOP rodam e registram seus servidores. O runtime
    // retorna na hora com client->server = 0 se o IRX ainda não registrou o SID.
    // A maioria dos jogos repete o bind em loop, mas o DRA2 tenta uma vez só
    // (0x003EB204, SID 0x11 do IOPHOOEY.IRX) e dá "Couldn't bind io system rpc.".
    // Aqui o bind bloqueante espera: deixa passar tempo de EE em fatias de 1 ms até o
    // servidor existir (limite de 2 s), e só então faz o bind HLE.
    void sifBindRpcBlocking(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sid = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);

        if (runtime && (mode & 1u) == 0u)
        {
            constexpr uint64_t kSliceCycles = 294912000ull / 1000ull; // 1 ms de EE
            constexpr int kMaxSlices = 2000;
            int slices = 0;
            // accountCycles é a API pública que faz o tempo do EE passar: avança
            // os timers e o IOP emulado, como se o EE estivesse esperando no console.
            while (!PS2IopTransport::canBindRpc(runtime, sid) && slices < kMaxSlices)
            {
                runtime->eeScheduler().accountCycles(static_cast<uint32_t>(kSliceCycles));
                ++slices;
            }
            if (slices > 0)
            {
                std::fprintf(stderr, "[dra2] sceSifBindRpc sid=0x%X: esperou %d ms pelo IOP%s\n",
                             sid, slices, slices >= kMaxSlices ? " (TIMEOUT)" : "");
            }
        }

        ctx->pc = getRegU32(ctx, 31);
        ps2_stubs::sceSifBindRpc(rdram, ctx, runtime);
    }

    // 0x003F2CD0: scePadPortOpen(port, slot, dmaBuf).
    // O DRA2 suporta multitap (4 jogadores) e abre os 8 controles no boot, com
    // port = i & 1 e slot = i >> 1 (0x003EDDF8), dando erro fatal se qualquer um
    // falhar. No console, com o MTAPMAN carregado, abrir um slot não exige que
    // haja controle nele. O HLE de pad do runtime só tem o slot 0, então aqui os
    // slots 1 a 3 abrem com sucesso. O resto do HLE (GetState, Read, Info*) já
    // reporta "desconectado" para eles.
    void padPortOpenMultitap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t slot = getRegU32(ctx, 5);
        ctx->pc = getRegU32(ctx, 31);
        if (slot >= 1u && slot <= 3u)
        {
            setReturnS32(ctx, 1);
        }
        else
        {
            ps2_stubs::scePadPortOpen(rdram, ctx, runtime);
        }
    }

    // Diagnóstico (DRA2_TRACE_RPC=1): registra cada sceSifCallRpc (0x0040B338) com
    // client, fno, modo e duração. Mostra as primeiras 40 chamadas e depois um
    // resumo a cada 2000.
    void sifCallRpcTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool enabled = [] {
            const char *v = std::getenv("DRA2_TRACE_RPC");
            return v && *v && *v != '0';
        }();
        static uint64_t count = 0;

        const uint32_t client = getRegU32(ctx, 4);
        const uint32_t fno = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31);
        const auto t0 = std::chrono::steady_clock::now();

        static uint64_t entries = 0;
        ++entries;
        if (enabled && (entries <= 40 || entries % 2000 == 0))
        {
            std::fprintf(stderr, "[dra2:rpc] -> entrada #%llu client=0x%X fno=0x%X mode=%u ra=0x%X\n",
                         static_cast<unsigned long long>(entries), client, fno, mode, ra);
        }

        ctx->pc = ra;
        ps2_syscalls::sceSifCallRpc(rdram, ctx, runtime);

        if (enabled)
        {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            ++count;
            if (count <= 40 || count % 2000 == 0)
            {
                std::fprintf(stderr, "[dra2:rpc] #%llu client=0x%X fno=0x%X mode=%u ra=0x%X ret=%d %lldus\n",
                             static_cast<unsigned long long>(count), client, fno, mode, ra,
                             static_cast<int32_t>(getRegU32(ctx, 2)), static_cast<long long>(us));
            }
        }
    }

    // ---- DIAGNÓSTICO TEMPORÁRIO (DRA2_TRACE_GS=1) -------------------------------
    bool traceGs()
    {
        static const bool on = [] {
            const char *v = std::getenv("DRA2_TRACE_GS");
            return v && *v && *v != '0';
        }();
        return on;
    }

    void traceIntcCall(const char *name, R5900Context *ctx)
    {
        if (traceGs())
            std::fprintf(stderr, "[dra2:intc] %s(a0=0x%X, a1=0x%X, a2=0x%X, a3=0x%X) ra=0x%X\n", name,
                         getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                         getRegU32(ctx, 31));
    }

    void addIntcHandlerTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traceIntcCall("AddIntcHandler", ctx);
        ctx->pc = getRegU32(ctx, 31);
        ps2_syscalls::AddIntcHandler(rdram, ctx, runtime);
    }

    void enableIntcTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traceIntcCall("EnableIntc", ctx);
        ctx->pc = getRegU32(ctx, 31);
        ps2_syscalls::EnableIntc(rdram, ctx, runtime);
    }

    // Envolve o medidor de tempo do limitador de frame (0x003E5F50).
    void frameTimerTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t calls = 0;
        if (traceGs() && (++calls % 200000u) == 1u)
        {
            const GSRegisters &gs = runtime->memory().gs();
            const uint8_t *flag = getConstMemPtr(rdram, 0x5384F0u + 0x434u);
            std::fprintf(stderr, "[dra2:gs] calls=%llu csr=0x%llX imr=0x%llX flag434=%u count=0x%X\n",
                         static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(gs.csr.load()),
                         static_cast<unsigned long long>(gs.imr), flag ? *reinterpret_cast<const uint32_t *>(flag) : 0xFFFFFFFFu,
                         ctx->cop0_count);
        }
        FUN_003e5f50_0x3e5f50(rdram, ctx, runtime);
    }
    // ---------------------------------------------------------------------------

    struct SdkBinding
    {
        uint32_t address;
        const char *handler;
    };

    // Funções do SDK da Sony identificadas pelo ps2_analyzer, ligadas aos handlers HLE
    // do runtime. Gerado por tools/gen_sdk_bindings.py.
    constexpr SdkBinding kSdkBindings[] = {
#include "dra2_sdk_bindings.inc"
    };

    // Se DRA2_ISO apontar para a ISO original, o IOP emulado lê os setores dela
    // (LBNs reais do disco). Sem ela, monta um ISO virtual a partir do cdRoot
    // (a pasta do ELF).
    void configureDiscImage()
    {
        const char *iso = std::getenv("DRA2_ISO");
        if (!iso || !*iso)
        {
            return;
        }
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[dra2] cdImage = %s\n", iso);
    }

    // Resolve um handler HLE do runtime pelo nome, com a mesma regra do
    // bindAddressHandler do upstream (syscalls primeiro, depois stubs, com aliases).
    std::optional<PS2Runtime::RecompiledFunction> resolveHandler(std::string_view name)
    {
        const std::string_view syscall = ps2_runtime_calls::resolveSyscallName(name);
        if (!syscall.empty())
        {
#define DRA2_RESOLVE_SYSCALL(n) \
    if (syscall == std::string_view{#n}) return &ps2_syscalls::n;
            PS2_SYSCALL_LIST(DRA2_RESOLVE_SYSCALL)
#undef DRA2_RESOLVE_SYSCALL
        }
        const std::string_view stub = ps2_runtime_calls::resolveStubName(name);
        if (!stub.empty())
        {
#define DRA2_RESOLVE_STUB(n) \
    if (stub == std::string_view{#n}) return &ps2_stubs::n;
            PS2_STUB_LIST(DRA2_RESOLVE_STUB)
#undef DRA2_RESOLVE_STUB
        }
        return std::nullopt;
    }

    // Handlers dos bindings do SDK, por endereço de entrada da função.
    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> &sdkHandlers()
    {
        static std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> handlers;
        return handlers;
    }

    // Trampolim único dos bindings do SDK. O código gerado faz ctx->pc = alvo antes
    // de despachar o jal, então ctx->pc identifica a função. Igual aos stubs
    // gerados, faz ctx->pc = $ra antes de chamar o HLE. O bindAddressHandler do
    // upstream não faz isso, e o sceSifCallRpc com callback entra em laço.
    void sdkTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entry = ctx->pc;
        const auto it = sdkHandlers().find(entry);
        ctx->pc = getRegU32(ctx, 31);
        if (it == sdkHandlers().end())
        {
            std::fprintf(stderr, "[dra2] trampolim sem handler para 0x%08X\n", entry);
            setReturnS32(ctx, -1);
            return;
        }
        it->second(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime)
    {
        configureDiscImage();

        size_t bound = 0;
        for (const SdkBinding &b : kSdkBindings)
        {
            const auto handler = resolveHandler(b.handler);
            if (!handler)
            {
                std::fprintf(stderr, "[dra2] handler '%s' não existe no runtime (0x%08X)\n", b.handler, b.address);
                continue;
            }
            sdkHandlers()[b.address] = *handler;
            if (runtime.replaceFunction(b.address, sdkTrampoline))
            {
                ++bound;
            }
        }
        std::fprintf(stderr, "[dra2] %zu/%zu SDK bindings aplicados\n", bound, std::size(kSdkBindings));

        // Depois dos bindings do SDK, para sobrescrever os handlers genéricos.
        runtime.replaceFunction(0x00408B68u, ttyWrite);
        runtime.replaceFunction(0x0040B168u, sifBindRpcBlocking);
        runtime.replaceFunction(0x003F2CD0u, padPortOpenMultitap);
        runtime.replaceFunction(0x0040B338u, sifCallRpcTraced);
        if (traceGs())
        {
            runtime.replaceFunction(0x00408280u, addIntcHandlerTraced);
            runtime.replaceFunction(0x004082E0u, enableIntcTraced);
            runtime.replaceFunction(0x003E5F50u, frameTimerTraced);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Digimon Rumble Arena 2 (USA)", "SLUS_210.67", kEntry, kElfCrc32, apply);
