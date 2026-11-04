cmake_minimum_required(VERSION 3.21)

# Build nativo de Digimon Rumble Arena 2 (SLUS_210.67).
#
# Usa o runtime do PS2Recomp (external/PS2Recomp) como biblioteca e compila o
# código gerado (generated/) em targets separados, para iterar rápido:
#   dra2_generated  ~78k funções recompiladas (unity build + PCH, -O2)
#   dra2            executável: tabela de funções (-O0), overrides e main
#
# Mexer num override recompila só o override. Rodar o ps2_recomp de novo exige
# reconfigurar (cmake -S . -B build), porque o glob de generated/ é feito só na
# configuração. Isso é de propósito: com CONFIGURE_DEPENDS, cada build re-checaria 78k arquivos.

project(DRA2Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/PS2Recomp" CACHE PATH "Clone do PS2Recomp")
set(DRA2_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "Saída do ps2_recomp")
set(DRA2_UNITY_BATCH 32 CACHE STRING "Funções por TU no unity build do código gerado")

if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/CMakeLists.txt")
    message(FATAL_ERROR "PS2Recomp não encontrado em ${PS2RECOMP_DIR}")
endif()
if(NOT EXISTS "${DRA2_GENERATED_DIR}/register_functions.cpp")
    message(FATAL_ERROR "Código gerado não encontrado em ${DRA2_GENERATED_DIR}: rode o ps2_recomp antes")
endif()

# GCC/Clang no x86-64 precisam habilitar SSE4.1/AVX2 explicitamente (PS2Recomp#261).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT MSVC)
    add_compile_options(-march=x86-64-v3)
endif()

# Runtime e IOP do upstream. EXCLUDE_FROM_ALL: o ps2EntryRunner do upstream não é usado.
set(PS2X_IS_ARM_TARGET OFF)
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" ps2xIOP EXCLUDE_FROM_ALL)
add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" ps2xRuntime EXCLUDE_FROM_ALL)

set(RT_DIR "${PS2RECOMP_DIR}/ps2xRuntime")

# ---------------------------------------------------------------------------
# Código recompilado (sem register_functions.cpp)
# ---------------------------------------------------------------------------
file(GLOB DRA2_GENERATED_SOURCES "${DRA2_GENERATED_DIR}/*.cpp")
list(REMOVE_ITEM DRA2_GENERATED_SOURCES "${DRA2_GENERATED_DIR}/register_functions.cpp")
list(SORT DRA2_GENERATED_SOURCES)

add_library(dra2_generated STATIC ${DRA2_GENERATED_SOURCES})
target_include_directories(dra2_generated PUBLIC "${DRA2_GENERATED_DIR}")
target_link_libraries(dra2_generated PUBLIC ps2_runtime)
set_target_properties(dra2_generated PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE "${DRA2_UNITY_BATCH}"
)
target_precompile_headers(dra2_generated PRIVATE
    "${RT_DIR}/include/ps2_runtime_macros.h"
    "${RT_DIR}/include/ps2_runtime.h"
    "${DRA2_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${DRA2_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${RT_DIR}/include/ps2_syscalls.h"
    "${RT_DIR}/include/ps2_stubs.h"
)

# ---------------------------------------------------------------------------
# Executável
# ---------------------------------------------------------------------------
file(GLOB DRA2_OVERRIDE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/overrides/*.cpp")

add_executable(dra2
    "${RT_DIR}/src/main.cpp"
    "${DRA2_GENERATED_DIR}/register_functions.cpp"
    ${DRA2_OVERRIDE_SOURCES}
)
target_link_libraries(dra2 PRIVATE ps2_runtime dra2_generated)

# A tabela de funções é um construtor com ~360k atribuições. Com -O2 o GCC leva
# cerca de 28 min; -O0 basta, porque roda uma vez só, no boot.
set_source_files_properties("${DRA2_GENERATED_DIR}/register_functions.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0")

if(PS2X_ENABLE_DEBUG_UI)
    target_sources(dra2 PRIVATE "${RT_DIR}/src/lib/ps2_debug_panel.cpp")
    target_link_libraries(dra2 PRIVATE rlImGui imgui)
    target_compile_definitions(dra2 PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()
