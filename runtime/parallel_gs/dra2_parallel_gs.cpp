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
                for (uint32_t i = 0, n = nloop * nreg; i < n && off + 16 <= sizeBytes; i++, off += 16)
                    if (((tagHi >> ((i % nreg) * 4)) & 0xf) == 0x0e &&
                        (load64(off + 8) & 0xff) >= 0x50 && (load64(off + 8) & 0xff) <= 0x53)
                        regs[(load64(off + 8) & 0xff) - 0x50] = load64(off);
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
        info.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        info.force_progressive = true;
        info.anti_blur = true;
        info.adapt_to_internal_horizontal_resolution = true;
        auto scanout = m_gpu.vsync(info);
        if (scanout.image)
        {
            if (++m_stats.scanoutImages == 1)
                std::fprintf(stderr, "[dra2-gs] primeiro scanout %ux%u smode1=0x%llx\n",
                             scanout.image->get_width(), scanout.image->get_height(),
                             (unsigned long long)priv.smode1);
            if (m_present)
                readbackScanout(scanout);
        }
        else
            m_stats.emptyScanouts++;
        m_gpu.flush();
        m_device.next_frame_context();
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

    // Readback síncrono do scanout para a janela (raylib/OpenGL não compartilha
    // a imagem Vulkan). Um buffer de staging persistente é realocado só quando
    // o tamanho muda. Falha/formato inesperado mantém o quadro anterior vazio
    // e o runtime cai para a apresentação CPU.
    void readbackScanout(const ParallelGS::ScanoutResult &scanout)
    {
        const auto &image = *scanout.image;
        if (image.get_format() != VK_FORMAT_R8G8B8A8_UNORM)
        {
            m_stats.readbackFailures++;
            return;
        }
        const uint32_t w = image.get_width(), h = image.get_height();
        const size_t bytes = static_cast<size_t>(w) * h * 4;
        if (!m_readbackBuffer || m_readbackBytes != bytes)
        {
            Vulkan::BufferCreateInfo bufferInfo{};
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.domain = Vulkan::BufferDomain::CachedHost;
            m_readbackBuffer = m_device.create_buffer(bufferInfo);
            m_readbackBytes = bytes;
            if (!m_readbackBuffer)
            {
                m_stats.readbackFailures++;
                return;
            }
        }
        auto cmd = m_device.request_command_buffer();
        cmd->image_barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        cmd->copy_image_to_buffer(*m_readbackBuffer, image, 0, {}, {w, h, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        m_device.submit(cmd);
        m_device.wait_idle();
        const auto *rgba = static_cast<const uint8_t *>(
            m_device.map_host_buffer(*m_readbackBuffer, Vulkan::MEMORY_ACCESS_READ_BIT));
        if (!rgba)
        {
            m_stats.readbackFailures++;
            return;
        }
        // O scanout pode vir em supersample horizontal (ex.: 2560 para uma
        // largura interna de 640). Reduz por média de caixa pelo fator
        // inteiro até a largura interna antes de entregar à janela 640px.
        const uint32_t targetW = (scanout.internal_width && scanout.internal_width < w)
                                     ? scanout.internal_width : w;
        if (targetW < w && targetW && w % targetW == 0)
        {
            const uint32_t factor = w / targetW;
            m_presentFrame.assign(static_cast<size_t>(targetW) * h * 4, 0);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < targetW; x++)
                    for (unsigned ch = 0; ch < 4; ch++)
                    {
                        uint32_t sum = 0;
                        for (uint32_t k = 0; k < factor; k++)
                            sum += rgba[(static_cast<size_t>(y) * w + x * factor + k) * 4 + ch];
                        m_presentFrame[(static_cast<size_t>(y) * targetW + x) * 4 + ch] =
                            static_cast<uint8_t>(sum / factor);
                    }
            m_presentWidth = targetW;
        }
        else
        {
            m_presentFrame.assign(rgba, rgba + bytes);
            m_presentWidth = w;
        }
        m_device.unmap_host_buffer(*m_readbackBuffer, Vulkan::MEMORY_ACCESS_READ_BIT);
        m_presentHeight = h;
        if (++m_stats.presentedFrames == 1)
            std::fprintf(stderr, "[dra2-gs] primeiro quadro GPU apresentado %ux%u\n", w, h);
    }

    bool copyPresentationFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth,
                               uint32_t &outHeight) override
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_present || m_presentFrame.empty() || !m_presentWidth || !m_presentHeight)
            return false;
        outPixels = m_presentFrame;
        outWidth = m_presentWidth;
        outHeight = m_presentHeight;
        return true;
    }

    struct Stats { uint64_t gifPackets = 0, nativeFallbacks = 0, fifoRequests = 0, fifoBytes = 0, latches = 0, scanoutImages = 0, emptyScanouts = 0, presentedFrames = 0, readbackFailures = 0; } m_stats;

private:
    PS2Runtime &m_runtime;
    bool m_present = false;
    std::vector<uint8_t> m_presentFrame;
    uint32_t m_presentWidth = 0, m_presentHeight = 0;
    Vulkan::BufferHandle m_readbackBuffer;
    size_t m_readbackBytes = 0;
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
