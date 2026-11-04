// Game overrides de Digimon Rumble Arena 2 (SLUS_210.67, NTSC-U v1.02).
//
// Código nosso, ligado no ps2EntryRunner pelo tools/stage_runner.sh. Usa a API
// game_overrides.h do PS2Recomp, que só aplica estas correções quando o nome do
// ELF, o entry e o CRC32 batem.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <iterator>

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

    void apply(PS2Runtime &runtime)
    {
        size_t bound = 0;
        for (const SdkBinding &b : kSdkBindings)
        {
            if (ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
            {
                ++bound;
            }
        }
        std::fprintf(stderr, "[dra2] %zu/%zu SDK bindings aplicados\n", bound, std::size(kSdkBindings));

        runtime.registerFunction(0x00408B68u, ttyWrite);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Digimon Rumble Arena 2 (USA)", "SLUS_210.67", kEntry, kElfCrc32, apply);
