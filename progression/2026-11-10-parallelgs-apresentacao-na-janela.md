# paraLLEl-GS: scanout GPU apresentado na janela do jogo (`gpu-present`)

O objetivo desta sessão foi implementar de fato o Parallel-GS na janela:
até aqui o sink `gpu-shadow` executava `vsync` e descartava a imagem.
Agora `DRA2_GS_BACKEND=gpu-present` mostra o scanout Vulkan na janela
raylib do jogo. O CPU continua autoritativo para VRAM, readback ao EE e
SIGNAL/FINISH; só a imagem apresentada muda. Não houve promoção a backend
autoritativo, medição de FPS ou aceite visual de equivalência.

## Identidade (Ghidra)

O Ghidra aberto confirmou `SLUS_210.67`, linguagem `r5900:LE:32:default`,
entry `0x100008`, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
É observação estática; a causa implementada está no runtime/adaptador,
não numa rotina do jogo.

## Implementação em commits pequenos

- `91a5f86` (patch 0024, commit `3929fdd` no clone do runtime):
  `GSGifSink::copyPresentationFrame` opcional; após `onLatch`,
  `GS::latchHostPresentationFrame` usa o quadro do sink para a janela
  quando presente e fora do modo captura, mantendo o caminho CPU nos
  demais casos. O quadro é normalizado para a largura de apresentação
  de 640 px do frontend.
- `996a525`: modo `gpu-present` no adaptador. Após o `vsync`, readback
  síncrono do scanout para buffer de staging persistente (realocado só
  quando o tamanho muda) e entrega do RGBA. O mutex do sink virou
  recursivo porque o latch chama a cópia do quadro com o mutex tomado.
  `gpu-shadow` permanece com `presented=0`.
- `57ea02f`: o scanout ao vivo veio 2560×448 (largura interna 640,
  supersample horizontal). Sem redução, o frontend descartava o quadro;
  média de caixa pelo fator inteiro entrega 640×448.
- `94051c9`: o scanout traz alfa zero e a textura raylib modula por
  alfa — o quadro GPU sumia (janela preta com `presented=296`). Alfa
  forçado a opaco e log do brilho médio do primeiro quadro.

O readback síncrono por latch é o caminho simples e correto para esta
entrega; não há interop Vulkan↔OpenGL, fila de quadros nem zero-copy.

## Validação

- Build completo com `-DDRA2_ENABLE_PARALLEL_GS=ON`: passou (o rebuild
  das 2596 unidades unity levou ~35 min; só o warning herdado de
  `std::is_pod`).
- Execução real de 50 s com ISO original e `gpu-present`, sem entrada:
  `primeiro scanout 2560x448 smode1=0x740804504`; em 300 latches,
  `scanoutImages=296 emptyScanouts=4 presented=296 readbackFailures=0`.
- Captura da janela do jogo (grim, geometria pelo PID exato do `dra2`)
  aos 40 s mostra o túnel da abertura renderizado pela GPU. A captura
  CPU no mesmo instante mostra a mesma cena com as diferenças já
  conhecidas das divergências de rasterização (brilho médio em cinza
  0,0796 GPU contra 0,0814 CPU; fração acima de 8%: 34,7% contra 35,9%).
  Artefatos ignorados em `.cache/performance/parallelgs-present/`
  (`window-gpu-present.png`, `window-cpu.png`, `run-gpu-present.log`).
- Regressões: `gpu-shadow` por 50 s mantém `presented=0` e janela CPU;
  sem `DRA2_GS_BACKEND`, caminho CPU intacto (captura baseline acima);
  `tools/apply_upstream_patches.sh` reconhece os 24 patches.

## Próximo passo

Sinais SIGNAL/FINISH a partir da GPU e promoção a backend autoritativo
continuam pendentes, assim como um caminho de apresentação sem readback
síncrono por quadro e a redução das divergências restantes (checkpoints
5–6 da captura V3). A imagem GPU ainda não foi aceita como equivalente
à CPU nem ao hardware.
