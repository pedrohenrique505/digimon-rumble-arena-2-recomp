// Exercise the actual HLE with SDK mode IDs, including a mode transition.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include <cstdio>
#include <stdexcept>

// No guest execution is needed by this test.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 4;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

int main()
try
{
    PS2Runtime runtime;
    R5900Context ctx{};
    auto select = [&](unsigned mode, unsigned interlace, unsigned frame) {
        SET_GPR_U32(&ctx, 4, interlace);
        SET_GPR_U32(&ctx, 5, mode);
        SET_GPR_U32(&ctx, 6, frame);
        ps2_syscalls::SetGsCrt(nullptr, &ctx, &runtime);
    };
    select(0x02, 1, 1);
    auto &gs = runtime.memory().gs();
    if (((gs.smode1 >> 3) & 127) != 32 || ((gs.smode1 >> 13) & 3) != 2)
        throw std::runtime_error("NTSC HLE did not select analog NTSC in SMODE1");
    if (gs.smode2 != 3 || !(gs.pmode & 3) || (gs.smode1 & (3ull << 16)))
        throw std::runtime_error("invalid CRT state after NTSC HLE");
    gs.smode1 |= 1ull << 25;
    select(0x03, 0, 0);
    if (((gs.smode1 >> 13) & 3) != 3 || gs.smode2 != 0)
        throw std::runtime_error("PAL transition retained NTSC/interlace state");
    if (!(gs.smode1 & (1ull << 25)))
        throw std::runtime_error("CRT HLE lost the output colour encoding");
    select(0x02, 1, 0);
    if (((gs.smode1 >> 13) & 3) != 2 || gs.smode2 != 1)
        throw std::runtime_error("NTSC field transition failed");
    select(0x50, 0, 1);
    if (gs.smode1 != 0)
        throw std::runtime_error("unsupported CRT mode retained a stale analog mode");
    std::puts("CRT HLE NTSC/PAL/transitions OK");
    return 0;
}
catch (const std::exception &e)
{
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
