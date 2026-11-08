# 2026-11-07: falha Lua na entrada do menu e reprodução do checkpoint recursivo

## Estado confirmado

O autoload termina, a abertura renderiza o cenário animado e o logotipo completo
de Digimon Rumble Arena 2 aparece. **O menu de opções ainda não foi validado.**
Ao avançar pelo título, o jogo tenta chamar o método virtual de um item nulo
em `0x252acc` (`FUN_00252948`).

A captura `/tmp/dra2-menu-state.bin`, feita antes da falha, mostra:

- objeto do menu em `0x01a59b80`, seleção 1, estado 2;
- vetores em `this+0x24` e `this+0x34` com início e fim iguais a zero;
- objetos visuais criados, mas nomes de recurso terminando em `(null)`;
- ausência da global Lua `menu`.

O script original `lua/menu`, extraído localmente de `LUA.RES`, define cinco
opções no carrossel principal. O script, seus recursos e as capturas permanecem
fora do git, em `.cache/ps2-lua/` e `/tmp`.

## Erro do compilador Lua

A RAM contém o erro de compilação de `lua/menu`, linha 1736:
`control structure too long`. O retorno da rotina de erro é `0x003cc190`.
Isso interrompe a criação da tabela `menu`; o C++ recebe contagens/defaults
vazios e acaba tentando ativar uma opção inexistente.

As sondas de entrada de `luaK_exp2reg` em `/tmp/dra2-lua-error2.log` registraram:

- descritor da chave em `0x01ffe7c0`, `t=f=0xffffffff`;
- descritor do valor em `0x01ffe7e0`, `t=0xffffffff`, `f=0x00510000`;
- o valor inválido já está presente antes de `discharge2reg`.

O watchpoint em `/tmp/dra2-lua-lifetime.log` mostra inicialização de `f=-1`
no PC `0x3c22a0`, SP `0x01ffe720`, e posterior spill de s0 pelo lexer
no PC `0x3d04a4`, SP `0x01ffe7a0`. Um endereço fixo de pilha também é
reutilizado normalmente por chamadas diferentes: o watchpoint sozinho não
estabelece que o lexer é a causa. A auditoria estática não encontrou escrita
em `e.t/e.f` em `dischargevars` ou `discharge2reg`.

Referências primárias para interpretar a RAM:
[estruturas Lua 5.0](https://www.lua.org/source/5.0/lobject.h.html) e
[estado Lua 5.0](https://www.lua.org/source/5.0/lstate.h.html).
A análise do ELF foi feita pelo GhidraMCP com o perfil R5900. A identidade do
ELF e a limitação do REA estão no [diário da paginação](2026-11-07-paginacao-memory-card.md).

## Defeito reproduzido no dispatcher

`PS2Runtime::dispatchGuestBranch` converte PC inalterado na entrada do callee
para o PC de continuação do caller. Essa compatibilidade permite que handlers
HLE retornem implicitamente, mas também reconhece incorretamente este caso:

1. Uma chamada A alcança outra chamada A por recursão.
2. O scheduler pede checkpoint antes de executar a chamada mais profunda.
3. A pilha nativa começa a retornar, preservando o PC pendente na entrada de A.
4. O dispatcher externo vê `pc == entryPc` e continua como se A tivesse terminado,
   embora SP e registradores ainda pertençam à chamada mais profunda.

O teste novo reproduz o defeito: **1 de 2 casos falha**, confirmando que a
entrada recursiva é substituída pelo fallthrough. O segundo caso passa e
protege o comportamento de um HLE que posta evento, mas retorna normalmente.
Resultados: `/tmp/dra2-yield-red.log`.

O teste está preservado em
[`patches/pending/0013-recursive-checkpoint-regression.patch`](../patches/pending/0013-recursive-checkpoint-regression.patch)
e no branch local `dra2-recursive-yield-repro` do clone PS2Recomp.
Ele não faz parte dos patches aplicados automaticamente.
**A correção do dispatcher ainda não foi implementada.** Sua ligação causal
com a falha Lua é a hipótese principal, ainda sem execução corrigida que a confirme.

## Próxima implementação

Distinguir um checkpoint efetivamente cedido de um retorno HLE implícito.
A proposta auditada é contar os retornos `true` de `EeScheduler::checkpointDue`
e comparar esse contador antes/depois de executar o callee. Uma alteração do
contador deve propagar o yield antes da normalização de PC e da comparação
com fallthrough.

Consultar apenas `m_checkpointPending` é insuficiente: timeslice pode ceder
sem marcar essa flag, e um evento pode chegar durante um HLE sem que ele tenha
cedido. Também não usar `checkpointDue(0)` como consulta: `accountCycles`
avança pelo menos um ciclo.

Critérios de conclusão:

1. Os dois testes de reprodução passam, preservando o retorno implícito HLE.
2. `lua/menu` compila e cria a tabela com cinco opções.
3. A execução real atravessa o título e mostra opções de menu navegáveis.

## Checkpoint de versionamento

- Patch 0010: alinhamento LQ/SQ; a sessão anterior registrou 63 testes CodeGenerator.
- Patch 0011: paginação do memory card; 14 testes de I/O passaram nesta sessão.
- Patch 0012: preserva as alterações locais de diagnóstico GS/VIF/VU e a correção
  de ZTE desligado. Os contadores e dumps ainda são específicos desta investigação.
- Override `0x3a888c`: reutiliza o epílogo gerado equivalente para uma continuação
  ausente. A nova execução ultrapassou o ponto que parava sob REA/GDB.
- As sondas temporárias de Lua e de RAM do menu foram retiradas dos overrides.
- O override sem sondas temporárias foi recompilado e `build/dra2` foi ligado
  novamente após a limpeza. Não usar capturas incompletas do último tick como
  evidência de tela preta.
- Os 12 patches foram aplicados, em ordem, sobre `e563698` usando um índice
  temporário do git. O patch de reprodução pendente também passou no check
  separado; ele continua fora da aplicação automática.
