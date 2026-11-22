// Adaptador GIF pós-arbitragem -> paraLLEl-GS (modo shadow, etapa 3).
// Reaproveita as estratégias validadas no replay offline:
//  - upload IMAGE/nativo com bits % 64 != 0: fallback CPU sobre VRAM mapeada;
//  - leitura local->host: buffer residual de qwords + cauda sub-qword da VRAM
//    sincronizada (aqui apenas consumida/contada; quem serve o EE é o CPU);
//  - escrita direta e clear HLE: rotina CPU sobre VRAM mapeada após flush.
// Sinais SIGNAL/FINISH seguem gerados pelo frontend CPU; o sink não os duplica.
#include "dra2_parallel_gs.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "thread_id.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
constexpr uint32_t kVram = 4 * 1024 * 1024;

unsigned bitsPerPixel(unsigned psm)
{
    switch (psm)
    {
    case 0: case 0x30: return 32;
    case 1: case 0x31: return 24;
    case 2: case 0xa: case 0x32: case 0x3a: return 16;
    case 0x13: case 0x1b: return 8;
    case 0x14: case 0x24: case 0x2c: return 4;
    default: return 0;
    }
}

class ShadowSink final : public GSGifSink
{
public:
    ShadowSink(PS2Runtime &runtime, bool present) : m_runtime(runtime), m_present(present) {}

    bool ensureInit()
    {
        // Granite exige índice de thread registrado em toda thread que chama
        // Vulkan::Device/GSInterface. m_mutex serializa GameThread e a thread
        // de apresentação, então ambas compartilham o índice 0 com segurança.
        static thread_local bool s_threadRegistered = false;
        if (!s_threadRegistered)
        {
            Util::register_thread_index(0);
            s_threadRegistered = true;
        }
        if (m_ready)
            return true;
        if (m_failed)
            return false;
        m_context.set_num_thread_indices(1);
        if (!Vulkan::Context::init_loader(nullptr) ||
            !m_context.init_instance_and_device(nullptr, 0, nullptr, 0, 0))
        {
            std::fprintf(stderr, "[dra2-gs] Vulkan indisponível; sink GPU desligado\n");
            m_failed = true;
            return false;
        }
        m_device.set_context(m_context);
        m_device.init_frame_contexts(4);
        ParallelGS::GSOptions opts{};
        if (!m_gpu.init(&m_device, opts))
        {
            std::fprintf(stderr, "[dra2-gs] paraLLEl-GS sem requisitos; sink desligado\n");
            m_failed = true;
            return false;
        }
        const uint8_t *vram = m_runtime.memory().getGSVRAM();
        std::memcpy(m_gpu.map_vram_write(0, kVram), vram, kVram);
        m_gpu.end_vram_write(0, kVram);
        resetGpu();
        m_ready = true;
        std::fprintf(stderr, "[dra2-gs] sink paraLLEl-GS (shadow) ativo\n");
        return true;
    }

    void resetGpu()
    {
        m_gpu.reset_context_state();
        m_gpu.write_register(ParallelGS::RegisterAddr::PRMODECONT, uint64_t(1));
        for (unsigned i = 0; i < 2; i++)
        {
            m_gpu.write_register(ParallelGS::RegisterAddr(0x4c + i), uint64_t(10) << 16);
            m_gpu.write_register(ParallelGS::RegisterAddr(0x40 + i),
                                 (uint64_t(639) << 16) | (uint64_t(447) << 48));
        }
    }

    void cpuFallbackUpload(const uint64_t regs[4], const uint8_t *data, uint32_t size)
    {
        m_gpu.flush();
        auto *mapped = static_cast<uint8_t *>(m_gpu.map_vram_write(0, kVram));
        GSRegisters priv{};
        GS fallback;
        fallback.init(mapped, kVram, &priv);
        fallback.uploadImageNative(regs[0], regs[1], regs[2], regs[3], data, size);
        m_gpu.end_vram_write(0, kVram);
        m_stats.nativeFallbacks++;
    }

    void onGifPacket(const uint8_t *data, uint32_t sizeBytes) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        // Mesma inspeção de tags do replay: IMAGE parcial vai pelo fallback.
        auto load64 = [&](size_t at) { uint64_t v; std::memcpy(&v, data + at, 8); return v; };
        uint64_t regs[4] = {};
        size_t segStart = 0, off = 0;
        bool routed = false;
        while (off + 16 <= sizeBytes)
        {
            const uint64_t tagLo = load64(off), tagHi = load64(off + 8);
            const uint32_t nloop = tagLo & 0x7fff;
            const unsigned flg = (tagLo >> 58) & 3;
            uint32_t nreg = (tagLo >> 60) & 0xf;
            if (!nreg)
                nreg = 16;
            off += 16;
            if (flg == 0)
            {
                uint32_t adMask = 0;
                for (uint32_t r = 0; r < nreg; ++r)
                    if (((tagHi >> (r * 4)) & 0xf) == 0x0e)
                        adMask |= (1u << r);
                const size_t totalQwords = static_cast<size_t>(nloop) * nreg;
                if (adMask == 0)
                {
                    const size_t payloadBytes = totalQwords * 16;
                    off += (payloadBytes <= sizeBytes - off) ? payloadBytes : ((sizeBytes - off) & ~size_t(15));
                }
                else if (nreg == 1)
                {
                    for (uint32_t i = 0; i < nloop && off + 16 <= sizeBytes; ++i, off += 16)
                    {
                        const uint64_t addr = load64(off + 8) & 0xff;
                        if (addr >= 0x50 && addr <= 0x53)
                            regs[addr - 0x50] = load64(off);
                    }
                }
                else
                {
                    for (uint32_t loop = 0; loop < nloop && off + 16 <= sizeBytes; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg && off + 16 <= sizeBytes; ++r, off += 16)
                        {
                            if ((adMask >> r) & 1u)
                            {
                                const uint64_t addr = load64(off + 8) & 0xff;
                                if (addr >= 0x50 && addr <= 0x53)
                                    regs[addr - 0x50] = load64(off);
                            }
                        }
                    }
                }
            }
            else if (flg == 1)
            {
                uint32_t count = nloop * nreg;
                off += count * 8;
                if (count & 1)
                    off += 8;
            }
            else
            {
                uint32_t imageBytes = nloop * 16;
                if (off + imageBytes > sizeBytes)
                    imageBytes = sizeBytes - off;
                const unsigned bpp = bitsPerPixel((regs[0] >> 56) & 63);
                const uint64_t bits = (regs[2] & 0xfff) * ((regs[2] >> 32) & 0xfff) * bpp;
                if ((regs[3] & 3) == 0 && bpp && bits % 64)
                {
                    routed = true;
                    if (off - 16 > segStart)
                        m_gpu.gif_transfer(1, data + segStart, off - 16 - segStart);
                    cpuFallbackUpload(regs, data + off, imageBytes);
                    segStart = off + imageBytes;
                }
                off += imageBytes;
            }
        }
        if (!routed)
            m_gpu.gif_transfer(1, data, sizeBytes);
        else if (segStart < sizeBytes)
            m_gpu.gif_transfer(1, data + segStart, sizeBytes - segStart);
        m_stats.gifPackets++;
    }

    void onRegister(uint8_t regAddr, uint64_t value) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        if (regAddr >= 0x50 && regAddr <= 0x53)
            m_xferRegs[regAddr - 0x50] = value;
        m_gpu.write_register(ParallelGS::RegisterAddr(regAddr), value);
        if (regAddr == 0x53 && (value & 3) == 1)
        {
            const unsigned bpp = bitsPerPixel((m_xferRegs[0] >> 24) & 63);
            m_fifoTotal = ((m_xferRegs[2] & 0xfff) * ((m_xferRegs[2] >> 32) & 0xfff) * bpp) / 8;
            m_fifoQwords = m_fifoTotal / 16;
            m_fifoResidual.clear();
            m_fifoTailLoaded = false;
            m_fifoOracleReady = false;
        }
    }

    void onNativeUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
                        uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        const uint64_t regs[4] = {bitbltbuf, trxpos, trxreg, trxdir};
        for (unsigned i = 0; i < 4; i++)
            m_gpu.write_register(ParallelGS::RegisterAddr(0x50 + i), regs[i]);
        const unsigned bpp = bitsPerPixel((bitbltbuf >> 56) & 63);
        const uint64_t bits = (trxreg & 0xfff) * ((trxreg >> 32) & 0xfff) * bpp;
        if ((trxdir & 3) == 0 && bpp && bits % 64)
        {
            cpuFallbackUpload(regs, data, sizeBytes);
            return;
        }
        for (size_t offset = 0; offset < sizeBytes;)
        {
            size_t n = std::min<size_t>(sizeBytes - offset, 32767 * 16);
            std::vector<uint64_t> packet(2 + ((n + 15) / 16) * 2, 0);
            packet[0] = ((n + 15) / 16) | (uint64_t(1) << 15) | (uint64_t(2) << 58);
            std::memcpy(packet.data() + 2, data + offset, n);
            m_gpu.gif_transfer(1, packet.data(), packet.size() * 8);
            offset += n;
        }
    }

    void onLocalToHostRequest(uint32_t maxBytes) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        // Consome do FIFO GPU apenas para manter o estado do candidato em
        // sincronia; os bytes entregues ao EE continuam vindo do backend CPU.
        uint32_t served = 0;
        std::vector<uint8_t> scratch(maxBytes);
        while (served < maxBytes)
        {
            if (m_fifoResidual.empty())
            {
                if (m_fifoQwords)
                {
                    m_fifoResidual.resize(16);
                    m_gpu.read_transfer_fifo(m_fifoResidual.data(), 1);
                    m_fifoQwords--;
                }
                else if (!m_fifoTailLoaded)
                {
                    m_fifoTailLoaded = true;
                    const uint32_t tail = m_fifoTotal % 16;
                    if (!tail)
                        break;
                    // Cauda sub-qword: o candidato só entrega qwords. Reconstrói
                    // do oráculo CPU sobre a VRAM GPU sincronizada (replay).
                    if (!m_fifoOracleReady)
                    {
                        m_gpu.flush();
                        const auto *vram = static_cast<const uint8_t *>(m_gpu.map_vram_read(0, kVram));
                        m_fifoOracleVram.assign(vram, vram + kVram);
                        GSTransferCommand cmd{};
                        cmd.bitbltbuf.sbp = m_xferRegs[0] & 0x3fff;
                        cmd.bitbltbuf.sbw = (m_xferRegs[0] >> 16) & 0x3f;
                        cmd.bitbltbuf.spsm = (m_xferRegs[0] >> 24) & 0x3f;
                        cmd.bitbltbuf.dbp = (m_xferRegs[0] >> 32) & 0x3fff;
                        cmd.bitbltbuf.dbw = (m_xferRegs[0] >> 48) & 0x3f;
                        cmd.bitbltbuf.dpsm = (m_xferRegs[0] >> 56) & 0x3f;
                        cmd.trxpos.ssax = m_xferRegs[1] & 0x7ff;
                        cmd.trxpos.ssay = (m_xferRegs[1] >> 16) & 0x7ff;
                        cmd.trxpos.dsax = (m_xferRegs[1] >> 32) & 0x7ff;
                        cmd.trxpos.dsay = (m_xferRegs[1] >> 48) & 0x7ff;
                        cmd.trxpos.dir = (m_xferRegs[1] >> 59) & 3;
                        cmd.trxreg.rrw = m_xferRegs[2] & 0xfff;
                        cmd.trxreg.rrh = (m_xferRegs[2] >> 32) & 0xfff;
                        cmd.direction = 1;
                        m_fifoOracle.Initialize(m_fifoOracleVram.data(), kVram);
                        m_fifoOracle.BeginTransfer(cmd);
                        m_fifoOracleReady = true;
                    }
                    m_fifoResidual.assign(tail, 0);
                    std::vector<uint8_t> drain(m_fifoTotal - tail);
                    for (uint32_t drained = 0; drained < drain.size();)
                    {
                        uint32_t n = m_fifoOracle.ConsumeLocalToHostBytes(
                            drain.data() + drained, drain.size() - drained);
                        if (!n)
                            break;
                        drained += n;
                    }
                    for (uint32_t got = 0; got < tail;)
                    {
                        uint32_t n = m_fifoOracle.ConsumeLocalToHostBytes(
                            m_fifoResidual.data() + got, tail - got);
                        if (!n)
                            break;
                        got += n;
                    }
                }
                else
                    break;
            }
            uint32_t n = std::min<uint32_t>(m_fifoResidual.size(), maxBytes - served);
            std::memcpy(scratch.data() + served, m_fifoResidual.data(), n);
            m_fifoResidual.erase(m_fifoResidual.begin(), m_fifoResidual.begin() + n);
            served += n;
        }
        m_stats.fifoRequests++;
        m_stats.fifoBytes += served;
    }

    void onClear(uint32_t contextIndex, uint32_t rgba) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        auto &state = m_gpu.get_register_state().ctx[contextIndex != 0];
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
        m_gpu.flush();
        auto *mapped = static_cast<uint8_t *>(m_gpu.map_vram_write(0, kVram));
        GSCpuBackend clear;
        clear.Initialize(mapped, kVram);
        clear.ClearFramebuffer(c, rgba);
        m_gpu.end_vram_write(0, kVram);
    }

    void onDirectWrite(uint32_t psm, uint32_t base, uint32_t bw,
                       uint32_t x, uint32_t y, uint32_t value) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensureInit())
            return;
        m_gpu.flush();
        auto *mapped = static_cast<uint8_t *>(m_gpu.map_vram_write(0, kVram));
        GSCpuBackend writer;
        writer.Initialize(mapped, kVram);
        writer.WriteVram(psm, base, bw, x, y, value);
        m_gpu.end_vram_write(0, kVram);
    }

    void onReset() override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_ready)
            resetGpu();
    }

    void onLatch() override
    {
        bool submitted = false;
        size_t consumeSlot = kReadbackSlots;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (!ensureInit())
                return;
            const GSRegisters &priv = m_runtime.memory().gs();
            auto &p = m_gpu.get_priv_register_state();
            std::memcpy(&p.smode1, &priv.smode1, 8);
            std::memcpy(&p.pmode, &priv.pmode, 8);
            std::memcpy(&p.smode2, &priv.smode2, 8);
            std::memcpy(&p.dispfb1, &priv.dispfb1, 8);
            std::memcpy(&p.display1, &priv.display1, 8);
            std::memcpy(&p.dispfb2, &priv.dispfb2, 8);
            std::memcpy(&p.display2, &priv.display2, 8);
            std::memcpy(&p.bgcolor, &priv.bgcolor, 8);
            ParallelGS::VSyncInfo info{};
            info.phase = m_stats.latches & 1;
            info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
            info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            info.force_progressive = true;
            info.anti_blur = true;
            info.adapt_to_internal_horizontal_resolution = true;
            // The scanout samples VRAM and requires pending GS work to be submitted.
            m_gpu.flush();
            auto scanout = m_gpu.vsync(info);
            if (scanout.image)
            {
                if (++m_stats.scanoutImages == 1)
                    std::fprintf(stderr, "[dra2-gs] primeiro scanout %ux%u smode1=0x%llx\n",
                                 scanout.image->get_width(), scanout.image->get_height(),
                                 (unsigned long long)priv.smode1);
                if (m_present)
                {
                    submitted = enqueueReadbackUnlocked(scanout);
                    if (submitted)
                    {
                        const size_t prevSlot = (m_readbackWriteIdx + kReadbackSlots - 1) % kReadbackSlots;
                        const size_t olderSlot = m_readbackWriteIdx;
                        if (m_readbackRing[olderSlot].pending)
                            consumeSlot = olderSlot;
                        else if (m_stats.presentedFrames == 0 && m_readbackRing[prevSlot].pending)
                            consumeSlot = prevSlot;
                    }
                }
            }
            else
                m_stats.emptyScanouts++;
            m_stats.latches++;
            if (m_stats.latches % 300 == 0)
                std::fprintf(stderr,
                             "[dra2-gs] shadow latches=%llu gif=%llu nativeFallbacks=%llu fifoReq=%llu fifoBytes=%llu scanoutImages=%llu emptyScanouts=%llu presented=%llu readbackFailures=%llu\n",
                             (unsigned long long)m_stats.latches, (unsigned long long)m_stats.gifPackets,
                             (unsigned long long)m_stats.nativeFallbacks, (unsigned long long)m_stats.fifoRequests,
                             (unsigned long long)m_stats.fifoBytes,
                             (unsigned long long)m_stats.scanoutImages, (unsigned long long)m_stats.emptyScanouts,
                             (unsigned long long)m_stats.presentedFrames, (unsigned long long)m_stats.readbackFailures);
        }
        // Espera da fence, downsample e publicação rodam fora de m_mutex para
        // não travar a GameThread durante o readback do scanout.
        if (consumeSlot < kReadbackSlots)
            finishReadback(consumeSlot);
    }

    struct ReadbackSlot
    {
        Vulkan::BufferHandle buffer;
        Vulkan::Fence fence;
        size_t bytes = 0;
        uint32_t width = 0, height = 0, internalWidth = 0;
        bool pending = false;
    };

    bool enqueueReadbackUnlocked(const ParallelGS::ScanoutResult &scanout)
    {
        const auto &image = *scanout.image;
        if (image.get_format() != VK_FORMAT_R8G8B8A8_UNORM)
        {
            m_stats.readbackFailures++;
            return false;
        }
        auto &slot = m_readbackRing[m_readbackWriteIdx];
        if (slot.pending && slot.fence)
        {
            slot.fence->wait();
            slot.fence.reset();
            slot.pending = false;
        }
        const uint32_t w = image.get_width(), h = image.get_height();
        const size_t bytes = static_cast<size_t>(w) * h * 4;
        if (!slot.buffer || slot.bytes != bytes)
        {
            Vulkan::BufferCreateInfo bufferInfo{};
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
            slot.buffer = m_device.create_buffer(bufferInfo);
            slot.bytes = bytes;
            if (!slot.buffer)
            {
                m_stats.readbackFailures++;
                return false;
            }
        }
        auto cmd = m_device.request_command_buffer();
        cmd->copy_image_to_buffer(*slot.buffer, image, 0, {}, {w, h, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        m_device.submit(cmd, &slot.fence);
        slot.width = w;
        slot.height = h;
        slot.internalWidth = scanout.internal_width;
        slot.pending = true;
        m_readbackWriteIdx = (m_readbackWriteIdx + 1) % kReadbackSlots;
        return true;
    }

    void finishReadback(size_t slotIdx)
    {
        auto &slot = m_readbackRing[slotIdx];
        if (!slot.pending || !slot.buffer)
            return;
        if (slot.fence)
        {
            slot.fence->wait();
            slot.fence.reset();
        }
        slot.pending = false;

        const auto *rgba = static_cast<const uint8_t *>(
            m_device.map_host_buffer(*slot.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
        if (!rgba)
        {
            m_stats.readbackFailures++;
            return;
        }
        const uint32_t w = slot.width, h = slot.height;
        const uint32_t targetW = (slot.internalWidth && slot.internalWidth < w)
                                     ? slot.internalWidth : w;
        const bool isFirst = (m_stats.presentedFrames == 0);
        uint64_t rgbSum = 0;
        uint32_t outW = w;
        if (targetW < w && targetW && w % targetW == 0)
        {
            const uint32_t factor = w / targetW;
            m_presentScratch.resize(static_cast<size_t>(targetW) * h * 4);
            if (factor == 4)
            {
                for (uint32_t y = 0; y < h; y++)
                {
                    const uint8_t *src = rgba + static_cast<size_t>(y) * w * 4;
                    uint8_t *dst = m_presentScratch.data() + static_cast<size_t>(y) * targetW * 4;
                    for (uint32_t x = 0; x < targetW; x++)
                    {
                        const uint8_t *p = src + x * 16;
                        const uint8_t r = static_cast<uint8_t>((uint32_t(p[0]) + p[4] + p[8] + p[12]) >> 2);
                        const uint8_t g = static_cast<uint8_t>((uint32_t(p[1]) + p[5] + p[9] + p[13]) >> 2);
                        const uint8_t b = static_cast<uint8_t>((uint32_t(p[2]) + p[6] + p[10] + p[14]) >> 2);
                        dst[x * 4 + 0] = r;
                        dst[x * 4 + 1] = g;
                        dst[x * 4 + 2] = b;
                        dst[x * 4 + 3] = 0xff;
                    }
                }
            }
            else
            {
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < targetW; x++)
                    {
                        uint32_t r = 0, g = 0, b = 0;
                        const uint8_t *p = rgba + (static_cast<size_t>(y) * w + x * factor) * 4;
                        for (uint32_t k = 0; k < factor; k++)
                        {
                            r += p[k * 4 + 0];
                            g += p[k * 4 + 1];
                            b += p[k * 4 + 2];
                        }
                        uint8_t *dst = m_presentScratch.data() + (static_cast<size_t>(y) * targetW + x) * 4;
                        dst[0] = static_cast<uint8_t>(r / factor);
                        dst[1] = static_cast<uint8_t>(g / factor);
                        dst[2] = static_cast<uint8_t>(b / factor);
                        dst[3] = 0xff;
                    }
            }
            outW = targetW;
        }
        else
        {
            m_presentScratch.resize(slot.bytes);
            for (size_t i = 0; i + 3 < slot.bytes; i += 4)
            {
                m_presentScratch[i + 0] = rgba[i + 0];
                m_presentScratch[i + 1] = rgba[i + 1];
                m_presentScratch[i + 2] = rgba[i + 2];
                m_presentScratch[i + 3] = 0xff;
            }
            outW = w;
        }
        m_device.unmap_host_buffer(*slot.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);

        if (isFirst)
        {
            for (size_t i = 0; i + 3 < m_presentScratch.size(); i += 4)
                rgbSum += m_presentScratch[i] + m_presentScratch[i + 1] + m_presentScratch[i + 2];
            std::fprintf(stderr, "[dra2-gs] brilho médio do primeiro quadro GPU: %llu\n",
                         (unsigned long long)(m_presentScratch.empty() ? 0
                            : rgbSum / (m_presentScratch.size() / 4 * 3)));
            std::fprintf(stderr, "[dra2-gs] primeiro quadro GPU apresentado %ux%u\n", w, h);
        }
        static const bool traceFrames = std::getenv("DRA2_TRACE_SCANOUT") != nullptr;
        if (traceFrames)
        {
            // Hash the pixels published to the window, after downsampling.
            uint64_t hash = 14695981039346656037ull;
            for (size_t i = 0; i < m_presentScratch.size(); ++i)
            {
                hash ^= m_presentScratch[i];
                hash *= 1099511628211ull;
            }
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            std::fprintf(stderr, "[dra2-scanout] us=%lld frame=%llu size=%ux%u hash=%016llx\n",
                         static_cast<long long>(us),
                         static_cast<unsigned long long>(m_stats.presentedFrames),
                         outW, h, static_cast<unsigned long long>(hash));
        }
        {
            std::lock_guard<std::mutex> plock(m_presentMutex);
            m_presentFrame.swap(m_presentScratch);
            m_presentWidth = outW;
            m_presentHeight = h;
        }
        m_stats.presentedFrames++;
    }

    bool skipCpuRaster() const override { return m_present; }

    bool copyPresentationFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth,
                               uint32_t &outHeight) override
    {
        std::lock_guard<std::mutex> plock(m_presentMutex);
        if (!m_present || m_presentFrame.empty() || !m_presentWidth || !m_presentHeight)
            return false;
        outPixels = m_presentFrame;
        outWidth = m_presentWidth;
        outHeight = m_presentHeight;
        return true;
    }

    struct Stats { uint64_t gifPackets = 0, nativeFallbacks = 0, fifoRequests = 0, fifoBytes = 0, latches = 0, scanoutImages = 0, emptyScanouts = 0, presentedFrames = 0, readbackFailures = 0; } m_stats;

private:
    static constexpr size_t kReadbackSlots = 2;
    PS2Runtime &m_runtime;
    bool m_present = false;
    std::mutex m_presentMutex;
    std::vector<uint8_t> m_presentFrame;
    std::vector<uint8_t> m_presentScratch;
    uint32_t m_presentWidth = 0, m_presentHeight = 0;
    ReadbackSlot m_readbackRing[kReadbackSlots];
    size_t m_readbackWriteIdx = 0;
    Vulkan::Context m_context;
    Vulkan::Device m_device;
    ParallelGS::GSInterface m_gpu;
    bool m_ready = false, m_failed = false;
    std::recursive_mutex m_mutex;
    uint64_t m_xferRegs[4] = {};
    uint32_t m_fifoTotal = 0, m_fifoQwords = 0;
    std::vector<uint8_t> m_fifoResidual;
    std::vector<uint8_t> m_fifoOracleVram;
    GSCpuBackend m_fifoOracle;
    bool m_fifoOracleReady = false;
    bool m_fifoTailLoaded = false;
};
ShadowSink *g_sink = nullptr;
} // namespace

namespace dra2_parallel_gs
{
bool installIfRequested(PS2Runtime &runtime)
{
    const char *backend = std::getenv("DRA2_GS_BACKEND");
    const bool present = backend && !std::strcmp(backend, "gpu-present");
    if (!present && (!backend || std::strcmp(backend, "gpu-shadow")))
        return false;
    static ShadowSink sink(runtime, present);
    g_sink = &sink;
    runtime.gs().setGifSink(&sink);
    std::fprintf(stderr, "[dra2-gs] DRA2_GS_BACKEND=%s: sink instalado\n", backend);
    return true;
}
} // namespace dra2_parallel_gs
