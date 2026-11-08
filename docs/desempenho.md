# Desempenho: medição e plano incremental

## Escopo desta rodada (2026-11-07)

1. **Concluído:** reproduzir a lentidão, medir o avanço dos quadros e localizar
   o custo com REA/GDB e o Ghidra conectado.
2. **Concluído:** otimizar o arredondamento bilinear do GS, conferir equivalência,
   medir antes/depois e manter a mudança como patch local.

**Prioridade revisada após esta rodada:** medir o teto de ganho sem rasterização,
provar a integração de um GS acelerado existente e atacar VU conforme o perfil.
O roadmap vigente está em [NEXTSTEPS.md](../NEXTSTEPS.md). A redução local de
trabalho em `WritePixel`/`SampleTexture` deixa de ser o próximo objetivo principal.
As duas etapas concluídas acima são históricas; a numeração do novo roadmap
identifica o trabalho estrutural ainda pendente.

O jogo continua lento. Esta rodada não demonstra desempenho de uma partida,
60 FPS nem correção dos defeitos visuais conhecidos do carrossel.

## Medir com REA

Pré-requisitos: Linux x86-64, Node/npm, GDB com Python, sessão gráfica,
`build/dra2` e o ELF original. O script usa REA 6.8.0 por `npx`, inicia uma
sessão MCP temporária e fecha seu GDB ao terminar; não muda a configuração do
cliente nem usa o projeto Ghidra da GUI em headless.

```bash
DRA2_ISO='/caminho/para/o/disco.iso' \
  python3 tools/profile_game.py .cache/performance/minha-coleta
# Para medir um executável anterior preservado:
DRA2_ISO='/caminho/para/o/disco.iso' \
  python3 tools/profile_game.py .cache/performance/outra-coleta \
  --binary .cache/performance/dra2-before --samples 60 --interval 0.5
```

Cada coleta usa uma pasta nova. Salva identidade SHA-256 do executável/ELF,
evidências REA, pilhas de todas as threads, tempos, contador e resumo JSON.
O ELF precisa corresponder ao SHA-256 validado; o endereço do contador é
específico dessa versão. A duração de execução solicitada é limitada a 45 s
para caber na chamada ao debugger. A janela é encerrada ao fim da coleta.

O Ghidra identifica `FUN_003a2460` como parte do envio/troca de buffers:
a rotina envia DMA e incrementa `$gp+0x440`, isto é, `0x00538930` com
`$gp=0x005384F0`. Um breakpoint único na função nativa correspondente captura
RDRAM via `$rdi` (ABI x86-64 SysV). O script lê esse contador a cada amostra.
`guest_frames_per_second` mede o avanço desse contador de renderização na
abertura, entre a primeira e a última leitura disponível. Não é a frequência
de atualização da janela, nem prova de que cada quadro foi apresentado.

A coleta inclui o início da abertura e pausa todas as threads brevemente.
Use a mesma ISO, estado de memory card, ausência de entrada e carga da máquina
nas comparações. Não execute compilação ou outro benchmark simultaneamente.
`perf record` foi tentado com ciclos e `cpu-clock:u`, mas ambos foram bloqueados
pelo kernel; nenhuma configuração de permissões foi alterada.

## Otimização bilinear (patch 0014)

O filtro calculava quatro canais com `std::lround` por pixel. O patch mantém
a interpolação em float e converte o resultado com `int(double(value)+0.5)`.
No domínio dos canais interpolados, isso preserva o arredondamento e elimina
as chamadas libm. A soma usa double deliberadamente: `float + 0.5f` pode
arredondar prematuramente o float imediatamente abaixo de uma meia unidade.

```bash
tools/check_gs_bilinear.sh
```

O teste compila o backend real com `-O2`, sem arquivos do jogo. Compara
1.983.040 casos com a fórmula anterior, incluindo vizinhos de meias unidades,
e executa `Submit` com triângulos texturizados e filtragem bilinear. Confere
o hash FNV-1a de toda a VRAM contra a implementação original:
`1da861ea475b3918`. A inclusão do `.cpp` permite alcançar o helper interno
sem ampliar a interface pública do runtime. O patch precisa estar aplicado.

Em cinco execuções alternadas de cada versão, sem compilação concorrente:

| Medida | Antes | Depois |
| --- | ---: | ---: |
| Mediana de 80 triângulos de benchmark | 1,24762 s | 1,07442 s |
| Avanço do contador do jogo, coleta de ~30 s | 7,79/s | 8,10/s e 8,18/s |

O benchmark ficou 13,9% mais rápido em tempo, ou 16,1% em vazão. O ganho
observado no contador do jogo foi de aproximadamente 4–5%; uma coleta inicial
e duas finais não permitem separar completamente a variação entre execuções. O cenário
sintético não representa a distribuição de primitivas de uma partida.

Fontes primárias: [estado e arquitetura do PS2Recomp](https://github.com/ran-j/PS2Recomp)
e [semântica de lround](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/lround-lroundf-lroundl-llround-llroundf-llroundl).
