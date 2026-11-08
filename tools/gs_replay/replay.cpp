// Offline proof only: never selected by the game's renderer.
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include <array>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef DRA2_PARALLEL_GS
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#endif
using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
static constexpr uint32_t VRAM = 4 * 1024 * 1024;
struct Record
{
    uint32_t kind;
    std::vector<uint8_t> data;
};
static std::vector<Record> load(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, "DRA2GS02", 8))
        throw std::runtime_error("invalid capture header");
    std::vector<Record> records;
    size_t bytes = 0;
    for (;;)
    {
        uint32_t h[2];
        f.read(reinterpret_cast<char *>(h), 8);
        if (f.gcount() == 0 && f.eof())
            break;
        if (f.gcount() != 8 || h[0] > 11 || h[1] > 16 * 1024 * 1024 ||
            (bytes += h[1] + 8) > 512 * 1024 * 1024)
            throw std::runtime_error("invalid/truncated capture record");
        Record r{h[0], std::vector<uint8_t>(h[1])};
        if (h[1] && !f.read(reinterpret_cast<char *>(r.data.data()), h[1]))
            throw std::runtime_error("truncated capture payload");
        if (records.size() >= 2000000)
            throw std::runtime_error("capture has too many records");
        records.push_back(std::move(r));
    }
    if (records.empty() || records.front().kind != 0 || records.front().data.size() != VRAM)
        throw std::runtime_error("missing initial VRAM");
    return records;
}
template <typename T, size_t N> static std::array<T, N> values(const Record &r)
{
    if (r.data.size() != sizeof(T) * N)
        throw std::runtime_error("invalid event size");
    std::array<T, N> v;
    std::memcpy(v.data(), r.data.data(), r.data.size());
    return v;
}
static void save(const std::string &path, const void *data, size_t bytes)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.write(static_cast<const char *>(data), bytes))
        throw std::runtime_error("output write failed");
}
static void saveDisplay(const std::string &path, std::vector<uint8_t> &vram,
                        const std::array<uint64_t, 7> &display)
{
    // Raw display buffer comparison, before frontend preferred-source/deinterlace heuristics.
    const uint64_t fb = (display[0] & 2) ? display[4] : display[2];
    const uint32_t base = (fb & 511) * 32, bw = (fb >> 9) & 63, psm = (fb >> 15) & 31;
    GSCpuBackend reader;
    reader.Initialize(vram.data(), vram.size());
    std::vector<uint8_t> rgb(640 * 448 * 3);
    for (uint32_t y = 0; y < 448; y++)
        for (uint32_t x = 0; x < 640; x++)
        {
            uint32_t c = reader.ReadVram(psm, base, bw, x, y);
            size_t at = (y * 640 + x) * 3;
            if (psm == 2 || psm == 10)
            {
                rgb[at] = (c & 31) << 3;
                rgb[at + 1] = ((c >> 5) & 31) << 3;
                rgb[at + 2] = ((c >> 10) & 31) << 3;
            }
            else
            {
                rgb[at] = c;
                rgb[at + 1] = c >> 8;
                rgb[at + 2] = c >> 16;
            }
        }
    std::ofstream ppm(path, std::ios::binary);
    ppm << "P6\n640 448\n255\n";
    ppm.write(reinterpret_cast<char *>(rgb.data()), rgb.size());
    if (!ppm)
        throw std::runtime_error("image write failed");
}
int main(int argc, char **argv)
try
{
    if (argc != 3 && (argc != 4 || std::string(argv[3]) != "--checkpoints"))
        throw std::runtime_error("usage: dra2-gs-replay capture output-prefix [--checkpoints] (new files)");
    if (std::filesystem::exists(std::string(argv[2]) + ".vram") ||
        std::filesystem::exists(std::string(argv[2]) + ".ppm"))
        throw std::runtime_error("output prefix already exists");
    const bool checkpoints = argc == 4;
    const std::string checkpointDir = std::string(argv[2]) + ".checkpoints";
    if (checkpoints && !std::filesystem::create_directory(checkpointDir))
        throw std::runtime_error("checkpoint directory already exists");
    auto records = load(argv[1]);
    std::vector<uint8_t> vram = records.front().data;
    GSRegisters priv{};
    GS gs;
    gs.init(vram.data(), vram.size(), &priv);
    uint64_t frames = 0, references = 0, mismatchingReferences = 0, differentBytes = 0, readbacks = 0,
             clears = 0, directWrites = 0, nativeUploadFallbacks = 0;
    double work = 0, sync = 0, validation = 0;
    std::vector<uint8_t> result(VRAM);
    std::array<uint64_t, 7> display{};
#ifdef DRA2_PARALLEL_GS
    if (!Vulkan::Context::init_loader(nullptr))
        throw std::runtime_error("Vulkan loader unavailable");
    Vulkan::Context context;
    context.set_num_thread_indices(1);
    if (!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0))
        throw std::runtime_error("Vulkan device unavailable");
    Vulkan::Device device;
    device.set_context(context);
    device.init_frame_contexts(4);
    ParallelGS::GSInterface gpu;
    ParallelGS::GSOptions opts{};
    if (!gpu.init(&device, opts))
        throw std::runtime_error("paraLLEl-GS requirements unavailable");
    std::memcpy(gpu.map_vram_write(0, VRAM), vram.data(), VRAM);
    gpu.end_vram_write(0, VRAM);
    auto reset = [&]
    {
        gpu.reset_context_state();
        gpu.write_register(ParallelGS::RegisterAddr::PRMODECONT, uint64_t(1));
        for (unsigned i = 0; i < 2; i++)
        {
            gpu.write_register(ParallelGS::RegisterAddr(0x4c + i), uint64_t(10) << 16);
            gpu.write_register(ParallelGS::RegisterAddr(0x40 + i),
                               (uint64_t(639) << 16) | (uint64_t(447) << 48));
        }
    };
    reset();
#endif
    for (size_t index = 1; index < records.size(); index++)
    {
        const auto &r = records[index];
        auto start = Clock::now();
        switch (r.kind)
        {
        case 0:
            throw std::runtime_error("multiple initializations unsupported");
        case 1:
#ifdef DRA2_PARALLEL_GS
            if (r.data.size() % 16)
                throw std::runtime_error("GIF packet is not qword aligned");
            gpu.gif_transfer(1, r.data.data(), r.data.size());
#else
            gs.processGIFPacket(r.data.data(), r.data.size());
#endif
            break;
        case 2:
        {
            auto v = values<uint64_t, 2>(r);
#ifdef DRA2_PARALLEL_GS
            gpu.write_register(ParallelGS::RegisterAddr(v[0]), v[1]);
#else
            gs.writeRegister(v[0], v[1]);
#endif
            break;
        }
        case 3:
        {
            auto v = values<uint64_t, 4>(r);
            if (index + 1 == records.size() || records[index + 1].kind != 4)
                throw std::runtime_error("native upload missing payload");
            const auto &data = records[++index].data;
#ifdef DRA2_PARALLEL_GS
            // This revision truncates host transfer size to whole uint64_t words.
            // Preserve native uploads with a partial last word through the CPU transfer path.
            const unsigned psm = (v[0] >> 56) & 63;
            unsigned bpp = 0;
            switch (psm)
            {
            case 0: case 0x30: bpp = 32; break;
            case 1: case 0x31: bpp = 24; break;
            case 2: case 0xa: case 0x32: case 0x3a: bpp = 16; break;
            case 0x13: case 0x1b: bpp = 8; break;
            case 0x14: case 0x24: case 0x2c: bpp = 4; break;
            default: throw std::runtime_error("unsupported native upload PSM");
            }
            const uint64_t bits = (v[2] & 0xfff) * ((v[2] >> 32) & 0xfff) * bpp;
            const uint8_t addr[] = {0x50, 0x51, 0x52, 0x53};
            for (unsigned i = 0; i < 4; i++)
                gpu.write_register(ParallelGS::RegisterAddr(addr[i]), v[i]);
            if ((v[3] & 3) == 0 && bits % 64)
            {
                nativeUploadFallbacks++;
                gpu.flush();
                auto *mapped = static_cast<uint8_t *>(gpu.map_vram_write(0, VRAM));
                GSRegisters fallbackPriv{};
                GS fallback;
                fallback.init(mapped, VRAM, &fallbackPriv);
                fallback.uploadImageNative(v[0], v[1], v[2], v[3], data.data(), data.size());
                gpu.end_vram_write(0, VRAM);
                break;
            }
            for (size_t offset = 0; offset < data.size();)
            {
                size_t n = std::min<size_t>(data.size() - offset, 32767 * 16);
                std::vector<uint64_t> packet(2 + ((n + 15) / 16) * 2, 0);
                packet[0] = ((n + 15) / 16) | (uint64_t(1) << 15) | (uint64_t(2) << 58);
                std::memcpy(packet.data() + 2, data.data() + offset, n);
                gpu.gif_transfer(1, packet.data(), packet.size() * 8);
                offset += n;
            }
#else
            gs.uploadImageNative(v[0], v[1], v[2], v[3], data.data(), data.size());
#endif
            break;
        }
        case 4:
            throw std::runtime_error("orphan upload payload");
        case 5:
        {
            display = values<uint64_t, 7>(r);
            frames++;
#ifdef DRA2_PARALLEL_GS
            auto &p = gpu.get_priv_register_state();
            std::memcpy(&p.pmode, &display[0], 8);
            std::memcpy(&p.smode2, &display[1], 8);
            std::memcpy(&p.dispfb1, &display[2], 8);
            std::memcpy(&p.display1, &display[3], 8);
            std::memcpy(&p.dispfb2, &display[4], 8);
            std::memcpy(&p.display2, &display[5], 8);
            std::memcpy(&p.bgcolor, &display[6], 8);
            ParallelGS::VSyncInfo info{};
            info.phase = frames & 1;
            info.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            info.force_progressive = true;
            info.anti_blur = true;
            info.adapt_to_internal_horizontal_resolution = true;
            auto scanout = gpu.vsync(info);
            gpu.flush();
            device.next_frame_context();
#else
            priv.pmode = display[0];
            priv.smode2 = display[1];
            priv.dispfb1 = display[2];
            priv.display1 = display[3];
            priv.dispfb2 = display[4];
            priv.display2 = display[5];
            priv.bgcolor = display[6];
            gs.latchHostPresentationFrame();
#endif
            break;
        }
        case 6:
        {
            if (r.data.size() != VRAM)
                throw std::runtime_error("invalid reference VRAM");
#ifdef DRA2_PARALLEL_GS
            gpu.flush();
            auto t = Clock::now();
            const auto *p = static_cast<const uint8_t *>(gpu.map_vram_read(0, VRAM));
            sync += seconds(t);
            std::memcpy(result.data(), p, VRAM);
#else
            result = vram;
#endif
            auto vt = Clock::now();
            size_t different = 0;
            for (size_t i = 0; i < VRAM; i++)
                different += result[i] != r.data[i];
            references++;
            mismatchingReferences += different != 0;
            differentBytes += different;
            if (checkpoints)
            {
                const auto prefix = checkpointDir + "/" + std::to_string(references);
                saveDisplay(prefix + ".ppm", result, display);
                std::ofstream metadata(prefix + ".json");
                metadata << "{\"record\":" << index << ",\"latch\":" << frames
                         << ",\"different_bytes\":" << different << "}\n";
                if (!metadata)
                    throw std::runtime_error("checkpoint metadata write failed");
            }
            validation += seconds(vt);
            // Reference checking is excluded from replay work time.
            continue;
        }
        case 7:
        {
            auto v = values<uint32_t, 2>(r);
            clears++;
#ifdef DRA2_PARALLEL_GS
            // Preserve the runtime's HLE clear semantics; count the full CPU/GPU round trip.
            auto &state = gpu.get_register_state().ctx[v[0] != 0];
            uint64_t f = state.frame.bits, s = state.scissor.bits;
            GSContext c{};
            c.frame.fbp = f & 511;
            c.frame.fbw = (f >> 16) & 63;
            c.frame.psm = (f >> 24) & 63;
            c.frame.fbmsk = f >> 32;
            c.scissor.x0 = s & 2047;
            c.scissor.x1 = (s >> 16) & 2047;
            c.scissor.y0 = (s >> 32) & 2047;
            c.scissor.y1 = (s >> 48) & 2047;
            c.fba = state.fba.bits;
            gpu.flush();
            auto *p = static_cast<uint8_t *>(gpu.map_vram_write(0, VRAM));
            GSCpuBackend clear;
            clear.Initialize(p, VRAM);
            clear.ClearFramebuffer(c, v[1]);
            gpu.end_vram_write(0, VRAM);
#else
            gs.clearFramebufferContext(v[0], v[1]);
#endif
            break;
        }
        case 8:
        {
            auto v = values<uint32_t, 1>(r);
            if (v[0] > VRAM)
                throw std::runtime_error("oversized readback request");
            readbacks++;
#ifdef DRA2_PARALLEL_GS
            throw std::runtime_error("local-to-host GIF FIFO consumption needs explicit GPU adapter");
#else
            std::vector<uint8_t> data(v[0]);
            gs.consumeLocalToHostBytes(data.data(), data.size());
#endif
            break;
        }
        case 9:
        {
            auto v = values<uint32_t, 6>(r);
            directWrites++;
#ifdef DRA2_PARALLEL_GS
            // Use the existing swizzle implementation over synchronized host VRAM.
            // Mapping all pages is conservative; optimize only after correctness coverage.
            gpu.flush();
            auto *mapped = static_cast<uint8_t *>(gpu.map_vram_write(0, VRAM));
            GSCpuBackend writer;
            writer.Initialize(mapped, VRAM);
            writer.WriteVram(v[0], v[1], v[2], v[3], v[4], v[5]);
            gpu.end_vram_write(0, VRAM);
#else
            gs.WriteVram(v[0], v[1], v[2], v[3], v[4], v[5]);
#endif
            break;
        }
        case 10:
#ifdef DRA2_PARALLEL_GS
            reset();
#else
            gs.reset();
#endif
            break;
        case 11:
        {
            auto v = values<uint32_t, 1>(r);
            if (std::fesetround(v[0]))
                throw std::runtime_error("unsupported floating point mode");
            break;
        }
        default:
            throw std::runtime_error("unknown event");
        }
        work += seconds(start);
    }
    auto finish = Clock::now();
#ifdef DRA2_PARALLEL_GS
    gpu.flush();
    device.wait_idle();
    auto *mapped = gpu.map_vram_read(0, VRAM);
    std::memcpy(result.data(), mapped, VRAM);
    const char *backend = "parallel-gs";
#else
    result = vram;
    const char *backend = "cpu";
#endif
    sync += seconds(finish);
    save(std::string(argv[2]) + ".vram", result.data(), result.size());
    saveDisplay(std::string(argv[2]) + ".ppm", result, display);
    std::cout << "{\"backend\":\"" << backend << "\",\"records\":" << records.size()
              << ",\"latches\":" << frames << ",\"references\":" << references
              << ",\"mismatching_references\":" << mismatchingReferences
              << ",\"different_bytes\":" << differentBytes << ",\"clears\":" << clears
              << ",\"readbacks\":" << readbacks
              << ",\"direct_writes\":" << directWrites
              << ",\"native_upload_fallbacks\":" << nativeUploadFallbacks
              << ",\"work_seconds\":" << work
              << ",\"sync_seconds\":" << sync << ",\"validation_seconds\":" << validation << "}\n";
    #ifdef DRA2_PARALLEL_GS
    return references ? 0 : 2;
#else
    return !references ? 2 : (mismatchingReferences ? 1 : 0);
#endif
}
catch (const std::exception &e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
