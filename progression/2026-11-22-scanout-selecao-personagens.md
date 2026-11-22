# Quadros repetidos na Seleção de Personagens

No vídeo do PCSX2, a transição passa por `LOADING...` e a seleção
continua animando o fundo e o Agumon. No vídeo da recomp, o túnel para antes
do corte para a seleção e o conteúdo da seleção quase não muda. Entre 8 e
19 s, os 60 pares de quadros por segundo do vídeo da recomp têm diferença
média de luminância abaixo de 0,1 em praticamente todo segundo. A comparação
usa um recorte central reduzido para 140×170.

O replay agora inclui a confirmação na tela de seleção. O hash dos pixels
depois do readback mostrou um intervalo de cinco segundos com 300 scanouts
consecutivos sem mudança, apesar da atividade GIF. O Ghidra MCP confirmou
que `0x003a2460` prepara as listas de DMA e atualiza os registradores de
display antes de incrementar o contador em `$gp+0x440` (`0x538930`).
A sessão GNU GDB via REA confirmou o símbolo nativo
`GS::latchHostPresentationFrame()` no executável anterior ao rebuild
(evidência `ev_4b8770614e2fdc5c44b45ada5d6386e242697aa2ef80ba8c8dcb98f31a9d0006`).

O adaptador agora faz o flush do desenho antes de o scanout amostrar a
VRAM. O replayer upstream faz a mesma ordenação. `flush()` e `vsync()`
já avançam os contextos do dispositivo. O adaptador deixou de avançá-los
uma terceira vez e passou a manter oito contextos para reduzir as esperas
por reutilização. A imagem sai no layout da cópia para staging. [Replayer do paraLLEl-GS na revisão pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/dump/gs_dump_parser.cpp)

## Reprodução

Use um build com `DRA2_ENABLE_PARALLEL_GS=ON` e exporte `DRA2_ISO` para
a imagem original. Rode a partir da raiz do repositório.

```bash
DRA2_GS_BACKEND=gpu-present DRA2_TRACE_SCANOUT=1 \
  DRA2_PAD_REPLAY="$PWD/tools/scenarios/selecao-personagens.txt" \
  timeout -s INT 38s ./build/dra2 "$PWD/game/SLUS_210.67" > scanout.log 2>&1
python tools/check_scanout_motion.py scanout.log --start 24 --end 29
```

A janela é relativa ao primeiro scanout. Confira que o replay chegou à
seleção antes de interpretar o resultado. O hash identifica mudanças
exatas nos pixels publicados pelo sink e acrescenta trabalho por quadro.

| Build | Publicações/s (24–29 s) | Mudanças/s (24–29 s) | Maior espera por imagem nova |
| --- | ---: | ---: | ---: |
| Flush após o scanout | 60,0 | 0,0 | ≥ 5000 ms |
| Flush antes do scanout | 28,8 | 28,8 | 47,2 ms |
| Sem avanço adicional de contexto | 49,4 | 49,4 | 41,7 ms |
| Layout de readback direto e oito contextos | 60,0 | 60,0 | 18,1 ms |

Com oito contextos, a seleção manteve 60,0 mudanças/s entre 24 e 34 s,
com intervalo máximo de 18,5 ms. A janela de transição de 18 a 22 s teve
46,8 mudanças/s e um intervalo de 66,4 ms. A equivalência visual da
transição inteira com o PCSX2 ainda precisa de validação.

## Verificação do menu principal

Um replay separado termina antes de confirmar Multiplayer, deixando o
menu estabilizar. Entre 23 e 27 s do trace de chamadas a `0x003a2460`,
com o hash desligado, o contador guest avançou a 59,3/s no binário
anterior e a 61,8/s no corrigido. Os maiores intervalos entre chamadas
foram 30,2 e 21,5 ms. No binário final em `build/dra2`, com o hash
ligado, a mesma faixa de tempo relativa ao primeiro scanout registrou
58,2 quadros distintos/s, com intervalo máximo de 33,8 ms.
