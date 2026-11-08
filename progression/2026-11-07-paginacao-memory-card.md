# 2026-11-07: paginação do memory card e avanço da abertura

## Retomada

O executável `build/dra2` ainda era das 18:09, anterior ao patch 0010.
Os 248 objetos unity afetados por LQ/SQ já estavam compilados entre 18:21 e
18:28, mas faltava atualizar o arquivo estático e ligar o executável.
Foi completada a linkedição usando a lista exata de objetos de `build.ninja`.
Uma tentativa de build normal foi interrompida: a mudança de timestamps faria
recompilar também os objetos não afetados. Builds normais podem ainda executar
essa recompilação; não foram falsificados timestamps nem registros do Ninja.

REA 6.8.0 respondeu a `binary_session`, mas recusou `open_binary` do ELF
com `architecture_unsupported`: o perfil específico R5900 não é admitido.
Não foi alterado o ELF para contornar essa validação. O conector GhidraMCP
confirmou o alvo `SLUS_210.67`, linguagem `r5900:LE:32:default`, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de `0x428e80` confirmou o produtor de UNPACK descrito no diário
anterior. O backend GhidraMCP ficou indisponível numa consulta posterior.
Não há Evidence IDs de análise nativa REA para esse alvo.

## Novo bloqueio observado

Mesmo com o patch LQ/SQ completo ligado, o log repetia GetDir para
`/BASLUS-21067DRA2*`, sempre com resultado 1, até o jogo acusar falta de memória.
O HLE `sceMcGetDir` não lia o argumento mode em `$a3`: toda chamada retornava
a primeira página. O jogo continuava enumerando e acumulando o mesmo resultado.

O [contrato do PS2SDK](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/memorycard/include/libmc.h)
documenta mode=0 como início e demais valores como continuação.
O patch 0011 conserva a lista e o cursor por porta, avança o cursor em cada
página e retorna zero ao esgotar. Mode=0 reinicia a lista, e sceMcInit/End
limpam o estado da enumeração. Não há alteração forçada do estado do jogo.

## Validação

- 14 testes de I/O passaram (`/tmp/dra2-io-results-final.log`). O teste de
  GetDir verifica páginas 2+1, término com zero repetido e reinício com mode=0.
- Patch 0011 validado por `git apply --reverse --check` no clone local.
- Execução real com ISO e press/release de Enter: `/tmp/dra2-continue-mc`.
  O jogo mostrou "Autoloading data", "Load Successful" e a tela de copyright.
- O alvo FBP70, antes todo preto, tinha 25.530 pixels não pretos nos ticks
  1320–1800. Capturas FBP0 e FBP70 do tick 1800 mostram o copyright legível.
- ELF, código gerado e build permanecem ignorados pelo git. Capturas e logs
  estão em `/tmp`, fora do versionamento.

A execução até o título e a investigação posterior estão registradas abaixo.

## Continuação e investigação do menu

A execução longa `/tmp/dra2-menu-final` chegou ao título com logotipo completo
(tick 5400), mas a transição seguinte registrou JALR para zero em `0x252acc`.
O Ghidra, reativado pelo usuário, mostra que `FUN_00252948` seleciona um objeto
pela lista em `this+0x24` e chama o método virtual do slot `vtable+0x7c`.
A investigação posterior confirmou listas vazias e falha de compilação de `lua/menu`; veja o [diário Lua/checkpoint](2026-11-07-compilador-lua-e-checkpoint.md).

REA/GDB confirmou outro bloqueio dependente da retomada: `entry_003a886c`
termina com PC `0x3a888c`, que não consta da tabela gerada. O endereço tem
LD/JR/ADDIU de epílogo; a entrada gerada `0x3a8888` tem apenas NOP antes dele.
Um override registra `0x3a888c` reutilizando esse epílogo gerado, sem pular
lógica de menu. Evidência da parada: `ev_ab4bccc7149bc55cc31bae4a20223c8e9a90d8856f29ab6f0012145a4bcf089b`.
A alteração manual de PC na primeira reprodução foi somente diagnóstica e
não produziu uma execução validada até o menu.

Evidências REA exportadas para `.cache/rea-menu-20261107.json` (ignorado).
A sessão GDB `d401593e-4cbc-4a02-be87-84b0463383b4` terminou sem sincronizar
MI numa execução longa. `close_gdb_session` retornou `cleanup_incomplete`.
Os processos antigos, identificados por PID (GDB 153582, jogo 156939), foram
encerrados explicitamente; a execução nova de validação foi preservada.

A verificação por PID confirmou o encerramento dos processos antigos do REA.
