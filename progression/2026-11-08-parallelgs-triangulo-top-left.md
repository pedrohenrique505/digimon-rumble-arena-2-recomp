# Cobertura top-left e amostragem inteira de triângulos (registro 388)

A busca sequencial retomada após o registro 38 (`tools/find_gs_first_difference.py --start-record 388`)
confirmou que o prefixo `0..387` da captura V3 (`9c46301a57200e850f0e11802aa8692844df455a19dc19965c6d170976ebb2d5`)
é idêntico byte a byte nos 4 MiB de VRAM entre CPU e `paraLLEl-GS`
(SHA-256 comum `5cee0bad32b1cc1bd06458b6a668723835f3b896f16e03c410a0cb30abe6702b`).
A primeira divergência seguinte ocorre no registro **388**, um pacote GIF de
2.480 bytes com 16 primitivas `GS_PRIM_TRIANGLE` (oito quads de interface/fonte
compartilhando diagonais). O patch 0023 elimina todos os erros estruturais de
cobertura, dupla mistura na diagonal compartilhada e deslocamento de meio texel
nesse pacote e nos quatro primeiros checkpoints da captura, restando apenas
diferenças de arredondamento bilinear de 1–3 LSB no registro 388 (e 1 LSB RGB555
nos checkpoints 1–4). Checkpoints 5 e 6 continuam divergindo estruturalmente;
não houve aprovação visual de cena completa nem promoção do backend GPU.

## Cadeia causal do registro 388

O registro 388 usa `PRMODECONT=0`, `PRMODE=0x58` (`IIP=1`, `TME=1`, `ABE=1`),
`ALPHA_1=0x44`, textura `PSMT8` (`tbp0=0x3be0`, `tbw=4`, 256×256) com CLUT
`CSM1` `CT32` (`cbp=0x3ce0`) e filtro bilinear ativo em `TEX1`, desenhando em
framebuffer `CT32` (`fbp=0x78`, `fbw=10`) sob `FE_TOWARDZERO`. Os quads têm Y em
meio pixel na janela após `XYOFFSET` — por exemplo `(265, 407.5)–(278, 407.5)–(278, 386.5)`
e `(278, 386.5)–(265, 386.5)–(265, 407.5)` — e coordenadas `STQ` calibradas para
centros de texel em posições inteiras de pixel.

Em `GSCpuBackend::DrawTriangle` anterior ao patch 0023, três decisões divergiam
da grade do GS, do PCSX2 SW e do `paraLLEl-GS`:

1. `ofx` e `ofy` perdiam os 4 bits fracionários por `>> 4`;
2. cobertura e pesos baricêntricos avaliavam `(x + 0.5f, y + 0.5f)` com margem
   inclusiva `-1e-4f`, deslocando cobertura e `STQ` em `(-0.5, -0.5)` pixel e
   aplicando blending aditivo/alfa duas vezes sobre diagonais compartilhadas;
3. caixas delimitadoras `floor`/`ceil` sem regra top-left cobriam linhas/colunas
   externas às bordas direita e inferior.

A leitura direta dos texels `PSMT8` + `CSM1` em `.cache/performance/parallelgs-rec388/before-cpu.vram`
confirmou na textura usada pelo primeiro triângulo (`one-tri.gs`, SHA-256
`4b7c52b23e2e364a0ea5f0410eac5dbf0077309103202fb61dff44ec8478acbf`):

- linha `v=216`: `0x00000000` (transparente);
- linha `v=217` (`u=214..223`): `0xffffffff` (opaca), e `(u=224, v=217)` igual a
  `0xe1ffffff` (`A=225`).

Com o acréscimo indevido de `+0.5f`, a CPU interpolava `v=216.5` no topo do
glifo (misturando `v=216` e `v=217` para produzir `127/130` onde o `paraLLEl-GS`
amostrava `v=217` cheio) e cobria pixels fora da borda (`130/0` e `0/130`),
gerando 1.751 bytes divergentes no pacote completo (`after.gs`, SHA-256
`f4e317f9d71c4e249f174a972b757cc09c543f7b50e35c03f958c53353c39a73`) e 27 pixels
(105 bytes) em `one-tri.gs`.

## Reprodução sintética independente

`tools/test_gs_triangle_raster.py` (`8f540ae`) exercita o frontend GIF real sem
dados do jogo em 12 casos (3 geometrias × 4 modos de arredondamento `0`, `0x400`,
`0x800`, `0xc00`):

1. dois triângulos compartilhando diagonal sob blending aditivo (`0x20` uma vez,
   sem `0x40` na diagonal e sem cobertura nas bordas direita/inferior);
2. `XYOFFSET` com parte fracionária `0.5` pixel (`200.5`), exigindo ponto fixo 12.4;
3. triângulo texturizado bilinear com vértices em `Y` meio inteiro (`2.5..10.5`)
   e centros de texel alinhados às coordenadas inteiras do pixel.

Os 12 casos passaram no `dra2-gs-replay-vulkan` antes de qualquer mudança no CPU
e falharam no `dra2-gs-replay` anterior ao patch 0023.

## REA, Ghidra e fontes primárias

REA inspecionou `GSCpuBackend::DrawTriangle` por disassembly estático (`inspect_with_objdump`)
e observou a execução em GDB (`start_gdb_session` + `gdb_console` + `close_gdb_session`)
sobre o caso sintético `synth-bilinear.gs` (SHA-256
`33aace5bee63caa0e358c622b4d4b1afa1a6f0ab0a7e0944f32fcfa1d56a50b4`), encerrando
explicitamente o processo inferior com `kill` após o primeiro pixel e verificando
`target_state="none_observed"` no fechamento. Evidences integrais em
`.cache/performance/parallelgs-rec388/rea/`:

- `objdump-drawtriangle-before.json`: `ev_9cf0be138af42e0766de78112efdd5bb9ae31d379d589bf9975307ae00aa624e`
  (binário anterior `334c51f879e6af3ad29b9f9f9169f1d56abbadb723004e34ae7dc0de65e8223d`);
- `objdump-drawtriangle-after.json`: `ev_3ecbab3c27f2335c9e12a3ed6cc2da97b07359d5b00fe9fd6f8adb23dff79ade`
  (binário corrigido `aa5394dd1658488684ffa90c11cf81b179de78b02a87cd76a43b5025d24c5689`);
- `rea-gdb-before.json`: `ev_8a7839b8f53c2c30ef7c182c1d03eb68ede4b39145f706b32fd232766a943f6b` —
  observou `FIRST_SAMPLE s=0.187500 t=0.187500 q=1.000000` e `FIRST_WRITE x=2 y=2`
  (acima de `y0=2.5`);
- `rea-gdb-after.json`: `ev_24f7cce3ec6b744a525e8f8d3f6b3e1cd321147cc6b49b5caa9e6026b07ec3c3` —
  observou `FIRST_SAMPLE s=0.156250 t=0.218750 q=1.000000` (`2.5/16`, `3.5/16`) e
  `FIRST_WRITE x=2 y=3`.

Ghidra MCP confirmou `SLUS_210.67` (`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`)
e inspecionou `FUN_003a2340`, `FUN_003a4bc8` e `FUN_003a5e58` no anel de submissão
GIF (`0x558be0`), preservando a sessão GUI aberta sem atribuir o pacote a uma
rotina específica do jogo, pois a causa comprovada é do rasterizador nativo.

Fontes primárias corroboradas:

- `paraLLEl-GS` pinado em `cc6184af7e0c03da603045ca371ffa5dae9b0655`
  (`gs/shaders/triangle_setup.comp` e `gs/shaders/intersect.h`, avaliação inteira
  e regra top-left; `gs/shaders/ubershader.comp`, snap `1/16` de texel e `roundEven`);
- [PCSX2 GSRasterizer.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/SW/GSRasterizer.cpp),
  snapshot local `.cache/performance/parallelgs-rec388/pcsx2-GSRasterizer.cpp`,
  SHA-256 `a83f3c54c7754b4c384472bf8d284a03ccee9fdd70b2eefaa0ad67ff317f39ac`
  (`std::ceil` na grade inteira com regra top-left excluindo bordas direita e inferior);
- commit upstream de referência `ran-j/PS2Recomp` PR #364 (`ca799b3505952e76ee9673c130e2b41f28bdeed5`).

## Correção, validação e resíduos medidos

O patch 0023 (`157375a` no repositório raiz; commit `d30759c` em `external/PS2Recomp`,
árvore `0478243c13209c52522e072d5dc67711acc43dab` idêntica à aplicação sequencial
dos 23 patches sobre `e563698`) mantém vértices e `XYOFFSET` em ponto fixo 12.4,
avalia funções de aresta inteiras de 64 bits com desempate top-left e amostra
atributos nas coordenadas inteiras `(x, y)`, preservando `constantDepth` e
`constantColor` do patch 0021.

Resultados da suíte após rebuild de `.cache/gs-replay-build` (`dra2-gs-replay`
`aa5394dd1658488684ffa90c11cf81b179de78b02a87cd76a43b5025d24c5689`,
`dra2-gs-replay-vulkan` `355e2bd721e42b7078d9eeb10050bd96b7a10c2468bb27634e0a257c48c3b300`)
e religação de `build/dra2`:

- `tools/test_gs_triangle_raster.py`: 12/12 em CPU e Vulkan;
- `tools/test_gs_sprite_sampling.py`: 24/24 em CPU e Vulkan;
- `dra2-gs-test-depth` e `dra2-gs-test-constant-triangle`: 12/12 cada;
- `tools/test_gs_capture.py`: 16/16 em CPU e Vulkan (`--gpu`);
- `tools/test_gs_scanout.py` (CPU e Vulkan) e `build/dra2-test-gs-crt`: aprovados;
- `ps2_gs_tests.cpp` (`ps2_gs_tests_runner`): 74/74 aprovados;
- `tools/check_gs_bilinear.sh`: 1.983.040 comparações `lerpChannel`, asserção
  top-left e novo hash determinístico FNV-1a `0x5126770f04f8ef17`.

No caso reduzido `one-tri.gs`, 26 dos 27 pixels divergentes tornaram-se idênticos
byte a byte; resta apenas 1 pixel (`(277, 403)`, 4 bytes `114` vs `113`), onde
`225 * 0.5 = 112.5` arredonda para `113` em `lerpChannel` (`(113*130)>>7 = 114`)
e para `112` por `roundEven` no shader do `paraLLEl-GS` (`(112*130)>>7 = 113`).
No pacote completo do registro 388 (`after.gs`), os bytes divergentes caíram de
1.751 (erro máximo 255) para **447 bytes** com erro absoluto máximo **3**
(`(127,130)`, `(128,130)`, `(108,109)`, `(112,113)`, `(51,52)`, `(61,62)`),
todos explicados pela interpolação baricêntrica float sob `FE_TOWARDZERO` frente
ao snap `1/16` de texel + `roundEven` do `paraLLEl-GS`.

Uma comparação exploratória da captura histórica `complete.gs` (sem sobrescrever
suas referências antigas, salvas em `.cache/performance/parallelgs-rec388/replay-existing/`)
mostrou nos quatro primeiros checkpoints (`registros 421, 2258, 3173, 3592` /
`latches 30, 60, 90, 120`):

- queda do erro máximo de canal RGB de `248` para **`8`** (exatamente 1 LSB de
  canal de 5 bits em `CT16` expandido para 8 bits), com `18`, `4.660`, `7` e `52`
  pixels divergentes em `286.720`;
- nos checkpoints 5 e 6 (`registros 34057 e 65404` / `latches 150 e 180`),
  permanecem `200.067` e `201.202` pixels divergentes (`max_err = 152` e `248`).

## Próxima etapa

Isolar a primeira divergência **estrutural** (acima do resíduo bilinear de 1–3 LSB)
que surge entre o checkpoint 4 (`registro 3592`) e o checkpoint 5 (`registro 34057`),
preservando intactas as capturas anteriores.
