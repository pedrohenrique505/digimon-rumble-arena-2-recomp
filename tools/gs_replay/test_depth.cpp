// Synthetic GIF input: no game assets or captured packets.
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

static void check(uint64_t test, bool masked, bool draws, bool writesDepth)
{
    std::vector<uint8_t> vram(4 * 1024 * 1024);
    GSRegisters priv{};
    GS gs;
    gs.init(vram.data(), vram.size(), &priv);
    gs.writeRegister(0x4c, uint64_t(1) << 16); // FRAME: CT32, 64 pixels wide.
    gs.writeRegister(0x4e, uint64_t(1) | (uint64_t(masked) << 32));
    gs.writeRegister(0x47, test);
    constexpr uint32_t previousColor = 0x88776655, previousDepth = 0x100;
    constexpr uint32_t color = 0x44332211, depth = 0x200;
    gs.WriteVram(0, 0, 1, 0, 0, previousColor);
    gs.WriteVram(0x30, 32, 1, 0, 0, previousDepth);
    // PACKED RGBAQ + XYZ2, point topology from PRE, one vertex at (0, 0).
    const std::array<uint64_t, 6> packet = {
        (uint64_t(2) << 60) | (uint64_t(1) << 46) | 0x8001, 0x51,
        uint64_t(0x11) | (uint64_t(0x22) << 32),
        uint64_t(0x33) | (uint64_t(0x44) << 32), 0, depth};
    gs.processGIFPacket(reinterpret_cast<const uint8_t *>(packet.data()), sizeof(packet));
    if (gs.ReadVram(0, 0, 1, 0, 0) != (draws ? color : previousColor))
        throw std::runtime_error("unexpected framebuffer with TEST=" + std::to_string(test));
    if (gs.ReadVram(0x30, 32, 1, 0, 0) != (writesDepth ? depth : previousDepth))
        throw std::runtime_error("unexpected depth write with TEST=" + std::to_string(test));
}

int main() try
{
    for (unsigned method = 0; method < 4; method++)
    {
        check(uint64_t(method) << 17, false, true, false);
        check(uint64_t(method) << 17, true, true, false);
    }
    check(1 << 16, false, false, false); // ZTE=1, NEVER.
    check((1 << 16) | (1 << 17), false, true, true); // ALWAYS.
    check((1 << 16) | (1 << 17), true, true, false); // ZMSK.
    check((1 << 16) | (2 << 17), false, true, true); // GEQUAL passes.
    std::cout << "12 GIF depth cases passed\n";
    return 0;
}
catch (const std::exception &e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
