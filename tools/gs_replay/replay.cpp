// Offline proof only: never selected by the game's renderer.
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
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
static std::vector<Record> load(const char *path, bool &hasSmode1)
{
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    if (!f.read(magic, 8) || (std::memcmp(magic, "DRA2GS02", 8) && std::memcmp(magic, "DRA2GS03", 8)))
        throw std::runtime_error("invalid capture header");
    hasSmode1 = !std::memcmp(magic, "DRA2GS03", 8);
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
        if (h[0] == 5 && h[1] != (hasSmode1 ? 64u : 56u))
            throw std::runtime_error("invalid latch size for capture version");
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
#ifdef DRA2_PARALLEL_GS
// Diagnostic readback only. Never use this synchronous path for live presentation.
static void saveScanout(Vulkan::Device &device, const ParallelGS::ScanoutResult &scanout,
                        const std::string &prefix, uint64_t latch, bool assumeNtsc, uint64_t smode1)
{
    if (!scanout.image)
        throw std::runtime_error("last latch has no scanout image");
    const auto &image = *scanout.image;
    if (image.get_format() != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("unsupported scanout format");
    const uint32_t w = image.get_width(), h = image.get_height();
    Vulkan::BufferCreateInfo bufferInfo{};
    bufferInfo.size = size_t(w) * h * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
    auto buffer = device.create_buffer(bufferInfo);
    if (!buffer)
        throw std::runtime_error("scanout readback allocation failed");
    auto cmd = device.request_command_buffer();
    cmd->image_barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*buffer, image, 0, {}, {w, h, 1}, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    device.submit(cmd);
    device.wait_idle();
    const auto *rgba = static_cast<const uint8_t *>(
        device.map_host_buffer(*buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
    if (!rgba)
        throw std::runtime_error("scanout readback mapping failed");
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; i++)
        std::memcpy(rgb.data() + i * 3, rgba + i * 4, 3);
    device.unmap_host_buffer(*buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
    std::ofstream ppm(prefix + ".scanout.ppm", std::ios::binary);
    ppm << "P6\n" << w << ' ' << h << "\n255\n";
    ppm.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
    if (!ppm)
        throw std::runtime_error("scanout image write failed");
    std::ofstream meta(prefix + ".scanout.json");
    meta << "{\"latch\":" << latch << ",\"width\":" << w << ",\"height\":" << h
         << ",\"internal_width\":" << scanout.internal_width
         << ",\"internal_height\":" << scanout.internal_height
         << ",\"mode_width\":" << scanout.mode_width << ",\"mode_height\":" << scanout.mode_height
         << ",\"interlaced\":" << (scanout.interlaced ? "true" : "false")
         << ",\"smode1\":" << smode1
         << ",\"video_mode_source\":\"" << (assumeNtsc ? "explicit-ntsc-assumption" : "captured-smode1")
         << "\",\"force_progressive\":true,\"anti_blur\":true,\"visual_equivalence_approved\":false}\n";
    if (!meta)
        throw std::runtime_error("scanout metadata write failed");
}
#endif
int main(int argc, char **argv)
try
{
    if (argc < 3)
        throw std::runtime_error("usage: dra2-gs-replay capture output-prefix [--checkpoints] [--scanout | --scanout-ntsc] (new files)");
    bool checkpoints = false, exportScanout = false, assumeNtsc = false;
    for (int i = 3; i < argc; i++)
    {
        const std::string option = argv[i];
        if (option == "--checkpoints" && !checkpoints)
            checkpoints = true;
        else if ((option == "--scanout" || option == "--scanout-ntsc") && !exportScanout)
        {
            exportScanout = true;
            assumeNtsc = option == "--scanout-ntsc";
        }
        else
            throw std::runtime_error("unknown or repeated option: " + option);
    }
#ifndef DRA2_PARALLEL_GS
    if (exportScanout)
        throw std::runtime_error("scanout export requires the Vulkan replay");
#endif
    for (const char *suffix : {".vram", ".ppm", ".scanout.ppm", ".scanout.json"})
        if (std::filesystem::exists(std::string(argv[2]) + suffix))
            throw std::runtime_error("output prefix already exists");
    const std::string checkpointDir = std::string(argv[2]) + ".checkpoints";
    if (checkpoints && !std::filesystem::create_directory(checkpointDir))
        throw std::runtime_error("checkpoint directory already exists");
    bool hasSmode1 = false;
    auto records = load(argv[1], hasSmode1);
    if (exportScanout && !assumeNtsc && !hasSmode1)
        throw std::runtime_error("--scanout requires DRA2GS03 with SMODE1; legacy diagnostic: --scanout-ntsc");
    std::vector<uint8_t> vram = records.front().data;
    GSRegisters priv{};
    GS gs;
    gs.init(vram.data(), vram.size(), &priv);
    uint64_t frames = 0, references = 0, mismatchingReferences = 0, differentBytes = 0, readbacks = 0,
             clears = 0, directWrites = 0, nativeUploadFallbacks = 0, scanoutImages = 0, emptyScanouts = 0;
    double work = 0, sync = 0, validation = 0;
    std::vector<uint8_t> result(VRAM);
    std::array<uint64_t, 7> display{};
    uint64_t smode1 = 0;
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
    ParallelGS::ScanoutResult lastScanout{};
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
    // Local-to-host FIFO adapter state (GPU only). The candidate delivers
    // whole 16-byte words and zero-fills past the end; the runtime consumer
    // asks for arbitrary byte counts. Keep unconsumed bytes in a residual
    // buffer and rebuild the sub-word tail from synchronized VRAM.
    uint64_t xferRegs[4] = {};
    bool fifoActive = false;
    uint32_t fifoTotalBytes = 0, fifoQwordsLeft = 0;
    std::vector<uint8_t> fifoResidual;
    bool fifoTailLoaded = false;
    std::vector<uint8_t> oracleVram;
    GSCpuBackend fifoOracle, fifoVerify;
    bool fifoOracleReady = false;
    auto fifoSetup = [&]
    {
        const unsigned spsm = (xferRegs[0] >> 24) & 63;
        unsigned bpp = 0;
        switch (spsm)
        {
        case 0: case 0x30: bpp = 32; break;
        case 1: case 0x31: bpp = 24; break;
        case 2: case 0xa: case 0x32: case 0x3a: bpp = 16; break;
        case 0x13: case 0x1b: bpp = 8; break;
        case 0x14: case 0x24: case 0x2c: bpp = 4; break;
        default: throw std::runtime_error("unsupported local-to-host PSM");
        }
        fifoTotalBytes = ((xferRegs[2] & 0xfff) * ((xferRegs[2] >> 32) & 0xfff) * bpp) / 8;
        fifoQwordsLeft = fifoTotalBytes / 16;
        fifoResidual.clear();
        fifoTailLoaded = false;
        fifoOracleReady = false;
        fifoActive = true;
    };
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
            {
                // IMAGE uploads whose pixel bits are not a multiple of 64 lose
                // the last partial word in the candidate. Route those payloads
                // through the CPU transfer over synchronized VRAM.
                auto load64 = [&](size_t at) {
                    uint64_t v;
                    std::memcpy(&v, r.data.data() + at, 8);
                    return v;
                };
                uint64_t imgRegs[4] = {};
                size_t segStart = 0, off = 0;
                bool routed = false;
                while (off + 16 <= r.data.size())
                {
                    const uint64_t tagLo = load64(off);
                    const uint64_t tagHi = load64(off + 8);
                    const uint32_t nloop = tagLo & 0x7fff;
                    const unsigned flg = (tagLo >> 58) & 3;
                    uint32_t nreg = (tagLo >> 60) & 0xf;
                    if (!nreg)
                        nreg = 16;
                    off += 16;
                    if (flg == 0)
                    {
                        for (uint32_t i = 0, n = nloop * nreg; i < n && off + 16 <= r.data.size(); i++, off += 16)
                            if (((tagHi >> ((i % nreg) * 4)) & 0xf) == 0x0e &&
                                (load64(off + 8) & 0xff) >= 0x50 && (load64(off + 8) & 0xff) <= 0x53)
                                imgRegs[(load64(off + 8) & 0xff) - 0x50] = load64(off);
                    }
                    else if (flg == 1)
                    {
                        uint32_t count = nloop * nreg;
                        for (uint32_t i = 0; i < count && off + 8 <= r.data.size(); i++, off += 8)
                        {
                            unsigned reg = (tagHi >> ((i % nreg) * 4)) & 0xf;
                            // REGLIST addresses are register numbers, not A+D.
                            (void)reg;
                        }
                        if (count & 1)
                            off += 8;
                    }
                    else
                    {
                        uint32_t imageBytes = nloop * 16;
                        if (off + imageBytes > r.data.size())
                            imageBytes = r.data.size() - off;
                        const unsigned dpsm = (imgRegs[0] >> 56) & 63;
                        unsigned bpp = 0;
                        switch (dpsm)
                        {
                        case 0: case 0x30: bpp = 32; break;
                        case 1: case 0x31: bpp = 24; break;
                        case 2: case 0xa: case 0x32: case 0x3a: bpp = 16; break;
                        case 0x13: case 0x1b: bpp = 8; break;
                        case 0x14: case 0x24: case 0x2c: bpp = 4; break;
                        default: break;
                        }
                        const uint64_t bits = (imgRegs[2] & 0xfff) * ((imgRegs[2] >> 32) & 0xfff) * bpp;
                        if ((imgRegs[3] & 3) == 0 && bpp && bits % 64)
                        {
                            routed = true;
                            nativeUploadFallbacks++;
                            if (off - 16 > segStart)
                                gpu.gif_transfer(1, r.data.data() + segStart, off - 16 - segStart);
                            gpu.flush();
                            auto *mapped = static_cast<uint8_t *>(gpu.map_vram_write(0, VRAM));
                            GSRegisters fbPriv{};
                            GS fallback;
                            fallback.init(mapped, VRAM, &fbPriv);
                            fallback.uploadImageNative(imgRegs[0], imgRegs[1], imgRegs[2], imgRegs[3],
                                                       r.data.data() + off, imageBytes);
                            gpu.end_vram_write(0, VRAM);
                            segStart = off + imageBytes;
                        }
                        off += imageBytes;
                    }
                }
                if (!routed)
                    gpu.gif_transfer(1, r.data.data(), r.data.size());
                else if (segStart < r.data.size())
                    gpu.gif_transfer(1, r.data.data() + segStart, r.data.size() - segStart);
            }
#else
            gs.processGIFPacket(r.data.data(), r.data.size());
#endif
            break;
        case 2:
        {
            auto v = values<uint64_t, 2>(r);
#ifdef DRA2_PARALLEL_GS
            if (v[0] >= 0x50 && v[0] <= 0x53)
                xferRegs[v[0] - 0x50] = v[1];
            gpu.write_register(ParallelGS::RegisterAddr(v[0]), v[1]);
            if (v[0] == 0x53 && (v[1] & 3) == 1)
                fifoSetup();
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
            if (hasSmode1)
            {
                auto v = values<uint64_t, 8>(r);
                std::copy_n(v.begin(), 7, display.begin());
                smode1 = v[7];
            }
            else
                display = values<uint64_t, 7>(r);
            frames++;
#ifdef DRA2_PARALLEL_GS
            auto &p = gpu.get_priv_register_state();
            std::memcpy(&p.smode1, &smode1, 8);
            if (assumeNtsc)
            {
                // Legacy captures omit SMODE1. Explicit diagnostic assumption,
                // never silently infer a video mode from the ELF region.
                p.smode1.LC = ParallelGS::SMODE1Bits::LC_ANALOG;
                p.smode1.CMOD = ParallelGS::SMODE1Bits::CMOD_NTSC;
            }
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
            if (scanout.image)
                scanoutImages++;
            else
                emptyScanouts++;
            if (exportScanout)
                lastScanout = std::move(scanout);
            gpu.flush();
            device.next_frame_context();
#else
            priv.smode1 = smode1;
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
            if (!fifoActive)
                throw std::runtime_error("local-to-host consumption without transfer");
            if (!fifoOracleReady)
            {
                gpu.flush();
                oracleVram.assign(static_cast<const uint8_t *>(gpu.map_vram_read(0, VRAM)),
                                  static_cast<const uint8_t *>(gpu.map_vram_read(0, VRAM)) + VRAM);
                fifoOracle.Initialize(oracleVram.data(), VRAM);
                GSTransferCommand cmd{};
                cmd.bitbltbuf.sbp = xferRegs[0] & 0x3fff;
                cmd.bitbltbuf.sbw = (xferRegs[0] >> 16) & 0x3f;
                cmd.bitbltbuf.spsm = (xferRegs[0] >> 24) & 0x3f;
                cmd.bitbltbuf.dbp = (xferRegs[0] >> 32) & 0x3fff;
                cmd.bitbltbuf.dbw = (xferRegs[0] >> 48) & 0x3f;
                cmd.bitbltbuf.dpsm = (xferRegs[0] >> 56) & 0x3f;
                cmd.trxpos.ssax = xferRegs[1] & 0x7ff;
                cmd.trxpos.ssay = (xferRegs[1] >> 16) & 0x7ff;
                cmd.trxpos.dsax = (xferRegs[1] >> 32) & 0x7ff;
                cmd.trxpos.dsay = (xferRegs[1] >> 48) & 0x7ff;
                cmd.trxpos.dir = (xferRegs[1] >> 59) & 3;
                cmd.trxreg.rrw = xferRegs[2] & 0xfff;
                cmd.trxreg.rrh = (xferRegs[2] >> 32) & 0xfff;
                cmd.direction = 1;
                fifoOracle.BeginTransfer(cmd);
                fifoVerify.Initialize(oracleVram.data(), VRAM);
                fifoVerify.BeginTransfer(cmd);
                fifoOracleReady = true;
            }
            {
                // Serve from residual/FIFO/tail, then verify against the CPU
                // oracle stream, which mirrors the logical byte order.
                std::vector<uint8_t> data(v[0]);
                uint32_t served = 0;
                while (served < v[0])
                {
                    if (fifoResidual.empty())
                    {
                        if (fifoQwordsLeft)
                        {
                            fifoResidual.resize(16);
                            gpu.read_transfer_fifo(fifoResidual.data(), 1);
                            fifoQwordsLeft--;
                        }
                        else if (!fifoTailLoaded)
                        {
                            fifoTailLoaded = true;
                            const uint32_t tail = fifoTotalBytes % 16;
                            if (!tail)
                                break;
                            fifoResidual.assign(tail, 0);
                            std::vector<uint8_t> drain(fifoTotalBytes - tail);
                            for (uint32_t drained = 0; drained < drain.size();)
                            {
                                uint32_t n = fifoOracle.ConsumeLocalToHostBytes(
                                    drain.data() + drained, drain.size() - drained);
                                if (!n)
                                    throw std::runtime_error("fifo tail drain failed");
                                drained += n;
                            }
                            for (uint32_t got = 0; got < tail;)
                            {
                                uint32_t n = fifoOracle.ConsumeLocalToHostBytes(
                                    fifoResidual.data() + got, tail - got);
                                if (!n)
                                    throw std::runtime_error("fifo tail underflow");
                                got += n;
                            }
                        }
                        else
                            break;
                    }
                    uint32_t n = std::min<uint32_t>(fifoResidual.size(), v[0] - served);
                    std::memcpy(data.data() + served, fifoResidual.data(), n);
                    fifoResidual.erase(fifoResidual.begin(), fifoResidual.begin() + n);
                    served += n;
                }
                if (served != v[0])
                    throw std::runtime_error("fifo adapter short read");
                std::vector<uint8_t> expected(v[0]);
                if (fifoVerify.ConsumeLocalToHostBytes(expected.data(), v[0]) != v[0] ||
                    std::memcmp(data.data(), expected.data(), v[0]))
                    throw std::runtime_error("fifo adapter mismatch vs CPU");
            }
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
    double scanoutExport = 0;
#ifdef DRA2_PARALLEL_GS
    if (exportScanout)
    {
        auto start = Clock::now();
        uint64_t effectiveSmode1 = 0;
        std::memcpy(&effectiveSmode1, &gpu.get_priv_register_state().smode1, 8);
        saveScanout(device, lastScanout, argv[2], frames, assumeNtsc, effectiveSmode1);
        scanoutExport = seconds(start);
    }
#endif
    save(std::string(argv[2]) + ".vram", result.data(), result.size());
    saveDisplay(std::string(argv[2]) + ".ppm", result, display);
    std::cout << "{\"backend\":\"" << backend << "\",\"records\":" << records.size()
              << ",\"latches\":" << frames << ",\"references\":" << references
              << ",\"mismatching_references\":" << mismatchingReferences
              << ",\"different_bytes\":" << differentBytes << ",\"clears\":" << clears
              << ",\"readbacks\":" << readbacks
              << ",\"direct_writes\":" << directWrites
              << ",\"native_upload_fallbacks\":" << nativeUploadFallbacks
              << ",\"scanout_images\":" << scanoutImages << ",\"empty_scanouts\":" << emptyScanouts
              << ",\"scanout_export_seconds\":" << scanoutExport
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
