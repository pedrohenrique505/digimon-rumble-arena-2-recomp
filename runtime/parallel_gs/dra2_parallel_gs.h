#pragma once
// Etapa 3 (shadow): espelha o fluxo GIF pós-arbitragem no paraLLEl-GS.
// O backend CPU continua autoritativo para VRAM e apresentação; o sink GPU
// recebe o mesmo fluxo, na mesma ordem, sem alterar comportamento.
// Ativado apenas com DRA2_GS_BACKEND=gpu-shadow em build DRA2_ENABLE_PARALLEL_GS.
#include "runtime/gs/gs_frontend.h"
#include <cstdint>
#include <vector>

class PS2Runtime;

namespace dra2_parallel_gs
{
// Instala o sink se o ambiente pedir. Devolve true se instalado.
bool installIfRequested(PS2Runtime &runtime);
} // namespace dra2_parallel_gs
