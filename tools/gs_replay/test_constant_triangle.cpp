#include "runtime/gs/gs_cpu_backend.h"
#include <cfenv>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() try
{
    const int originalRounding = std::fegetround();
    unsigned cases = 0;
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
        for (uint8_t channel : {63, 128, 255})
        {
            if (std::fesetround(mode))
                throw std::runtime_error("rounding mode unavailable");
            std::vector<uint8_t> vram(4 * 1024 * 1024);
            GSCpuBackend backend;
            backend.Initialize(vram.data(), vram.size());
            GSPrimitiveBatch batch{};
            batch.vertexCount = 3;
            batch.state.prim.type = GS_PRIM_TRIANGLE;
            batch.state.prim.iip = true;
            auto &ctx = batch.state.context;
            ctx.frame.fbw = 1;
            ctx.scissor = {0, 63, 0, 63};
            ctx.zbuf.zbp = 4; // Separate from both 8-KiB framebuffer pages.
            ctx.zbuf.psm = GS_PSM_Z24;
            ctx.test = (1 << 16) | (1 << 17); // ZTE + ALWAYS.
            // Non-power-of-two area and subpixel coordinates expose weight
            // rounding. The attributes have zero gradients by construction.
            batch.vertices[0].x = 0; batch.vertices[0].y = 0;
            batch.vertices[1].x = 97.5f; batch.vertices[1].y = 3.25f;
            batch.vertices[2].x = 17.25f; batch.vertices[2].y = 89.75f;
            constexpr uint32_t depth = 0xbffe80;
            for (auto &v : batch.vertices)
            {
                v.r = v.g = v.b = v.a = channel;
                v.z = depth;
            }
            backend.Submit(batch);
            unsigned covered = 0;
            for (unsigned y = 0; y < 64; y++)
                for (unsigned x = 0; x < 64; x++)
                {
                    const auto color = backend.ReadVram(GS_PSM_CT32, 0, 1, x, y);
                    if (!color)
                        continue;
                    covered++;
                    if (color != uint32_t(channel) * 0x01010101u)
                        throw std::runtime_error("constant RGBA changed in rounding mode " + std::to_string(mode));
                    if (backend.ReadVram(GS_PSM_Z24, 128, 1, x, y) != depth)
                        throw std::runtime_error("constant Z changed in rounding mode " + std::to_string(mode));
                }
            if (covered < 1000)
                throw std::runtime_error("triangle did not exercise enough pixels");
            cases++;
        }
    std::fesetround(originalRounding);
    std::cout << cases << " constant triangle cases passed\n";
    return 0;
}
catch (const std::exception &e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
