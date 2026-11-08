// Testa o helper real (interno ao backend) e mede o caminho público Submit.
// Sem recursos do jogo. Compile com tools/check_gs_bilinear.sh.
#include "gs/gs_cpu_backend.cpp"
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>

static uint8_t reference(uint8_t a, uint8_t b, uint8_t c, uint8_t d, float x, float y)
{
    const float top = float(a) + (float(b) - float(a)) * x;
    const float bottom = float(c) + (float(d) - float(c)) * x;
    return clampU8(int(std::lround(top + (bottom - top) * y)));
}

int main() try
{
    std::mt19937 rng(0xD2A2);
    size_t checked = 0;
    const auto check = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d, float x, float y) {
        if (lerpChannel(a,b,c,d,x,y) != reference(a,b,c,d,x,y))
            throw std::runtime_error("bilinear divergiu de lround");
        ++checked;
    };
    // Meias unidades e os floats imediatamente vizinhos: float + 0.5f
    // arredonda prematuramente em alguns destes casos.
    const float weights[] = {0, std::nextafter(0.5f, 0.0f), 0.5f,
                             std::nextafter(0.5f, 1.0f), std::nextafter(1.0f, 0.0f)};
    for (unsigned a=0; a<256; ++a)
        for (unsigned b=0; b<256; ++b)
            for (float w : weights)
            {
                check(a,b,b,a,w,w);
                check(a,b,b,a,w,0.0f);
                check(a,b,b,a,w,1.0f);
            }
    for (unsigned i=0; i<1000000; ++i)
    {
        const uint32_t colors = rng();
        const float x = float(rng() & 0xFFFFFF) / 16777216.0f;
        const float y = float(rng() & 0xFFFFFF) / 16777216.0f;
        check(colors,colors>>8,colors>>16,colors>>24,x,y);
    }
    std::vector<uint8_t> vram(GSMem::MEMORY_SIZE);
    GSCpuBackend backend;
    backend.Initialize(vram.data(), vram.size());
    for (unsigned y=0; y<64; ++y)
        for (unsigned x=0; x<64; ++x)
            backend.WriteVram(GS_PSM_CT32,32,1,x,y,rng());
    GSPrimitiveBatch batch{};
    batch.vertexCount=3;
    auto &s=batch.state;
    s.prim.type=GS_PRIM_TRIANGLE;
    s.prim.tme=true;
    s.prim.fst=true;
    s.context.frame.fbp=200;
    s.context.frame.fbw=10;
    s.context.scissor={0,639,0,447};
    s.context.zbuf.zmask=true;
    s.context.tex0.tbp0=32;
    s.context.tex0.tbw=1;
    s.context.tex0.psm=GS_PSM_CT32;
    s.context.tex0.tcc=1;
    s.context.tex0.tfx=1;
    s.textureWidth=s.textureHeight=64;
    s.linearFilter=true;
    batch.vertices[0].x=0; batch.vertices[0].y=0;
    batch.vertices[1].x=640; batch.vertices[1].y=0; batch.vertices[1].u=63*16;
    batch.vertices[2].x=0; batch.vertices[2].y=448; batch.vertices[2].v=63*16;
    const auto start=std::chrono::steady_clock::now();
    for (int i=0; i<80; ++i) backend.Submit(batch);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    uint64_t hash=14695981039346656037ull;
    for (uint8_t b:vram) hash=(hash^b)*1099511628211ull;
    if (hash != 0x1da861ea475b3918ull)
        throw std::runtime_error("VRAM divergiu da rasterizacao de referencia");
    std::cout << "comparisons=" << checked << " raster_seconds=" << seconds
              << " vram_fnv1a=" << std::hex << hash << '\n';
}
catch (const std::exception &error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
