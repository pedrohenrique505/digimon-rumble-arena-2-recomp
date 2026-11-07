# 2026-11-07: tela e carga após os buracos no function map

## Execução
- Comando (65 s, `timeout -s INT 65`, saída `124` do timeout):
  `cd game && DRA2_TRACE_RPC=1 DRA2_ISO="$HOME/Downloads/Digimon Rumble Arena 2 (USA) - PlayStation 2/Digimon Rumble Arena 2 (USA).iso" ../build/dra2 "$PWD/SLUS_210.67"`
- A pasta real da ISO é `Digimon Rumble Arena 2 (USA) - PlayStation 2`; o `CLAUDE.md` foi corrigido nesta mesma sessão.
- Log completo (1089 linhas) em `/tmp/dra2_run.log` (temporário, fora do git).
- `98/98 SDK bindings aplicados`, nenhum erro, nenhum `missing-target`, nenhum `Couldn't bind`.

## Carga (RPC)
- Só 39 `sceSifCallRpc` completados (`[dra2:rpc] #1..#39`), todos no início, até antes do `tick=240`.
- Cliente dominante: `0x564D98` (servidor "io" do IOPHOOEY), ciclo `fno 5` (status) / `fno 2` / `fno 4`, `ra=0x3EA660/0x3EA3D0/0x3EA5F8`, todos `ret=0`.
- Depois do `tick=240` não há mais nenhum RPC: ou a carga inicial terminou, ou parou de pedir. O IOPHOOEY carregou (`io rpc registered`, threads de stream/disk/audio de pé).
- Laço principal estável até o fim: PC alternando `0x3A25DC`/`0x23D4C8`, `activeThreads=2`, `dma` 116 → 9254+, `gif` 20 → 935+ (tick 2880 no corte do log).

## Tela (captura do usuário)
- Não é mais só preto: faixa cinza fragmentada em tiras verticais no meio da tela, resto preto. Runtime `running`, `EE executor: guest`, PC `0x003EDC40` na captura.
- No log, sprites (`type=6`, `tme=1`) desenhados direto no FBP 0 em tiras verticais: `tex0=(tbp0=2240 tbw=10 psm=0x0 tw=10 th=10)`, `rgba=(128,128,128,128)`. `tbp0=2240` blocos = página 70, ou seja, é a cópia do alvo de render (FBP 70) para a tela descrita na entrada anterior.
- `[dbgstat]` estável por intervalo de 120 ticks: ~11k VIFcmds, ~14k draws, `tex=900`, `fbp{ 0:900 70:12520 280:920 }`. O grosso renderiza no FBP 70; FBP 0 e 280 recebem ~900 cada (cópia/apresentação).
- Suspeita (não confirmada): o conteúdo texturizado sai cinza uniforme/fragmentado. Falta conferir o que há de fato no FBP 70 antes da cópia e o formato/CLUT das texturas originais.

## Próximos passos
1. Investigar a faixa cinza: dumpar o FBP 70 (CT32) e o FBP 0 no mesmo instante e comparar com os PPM antigos em `game/fb*.ppm`; conferir se o problema está no desenho no FBP 70 ou na cópia/apresentação.
2. Desempenho do GS em software (pendente desde 2026-11-05).
