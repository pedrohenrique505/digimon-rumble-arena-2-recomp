# 2026-11-07: propagação de checkpoint recursivo

## Correção confirmada no runtime

O teste pendente foi reexecutado antes da alteração: 1 passou e 1 falhou.
O dispatcher confundia uma pausa na entrada de uma chamada recursiva com o
retorno implícito de um HLE. Isso substituía o PC pendente pela continuação
de um chamador cuja pilha já não correspondia ao contexto EE.

O patch `0013-propagate-recursive-checkpoint-yields.patch` adiciona uma geração
monotônica de checkpoints efetivamente tomados. `checkpointDue` incrementa-a
em todos os retornos `true`, incluindo vencimento de timeslice. O dispatcher
compara a geração antes/depois do callee e propaga a pausa antes de normalizar
o PC ou reconhecer o fallthrough. Postar evento sem tomar checkpoint continua
permitindo o retorno implícito HLE. O contador é usado somente pelo executor EE.

Validação:

- 3/3 regressões: recursão, coincidência com fallthrough e evento sem yield.
- 35/35 testes de expansão do runtime.
- Os 13 patches aplicam em sequência a um índice temporário de `e563698`.
- O script de aplicação reconhece todos os patches já aplicados; a verificação
  reversa ignora contexto alterado por patches posteriores (`-C0`). Uma falha
  de aplicação agora interrompe o script.

O clone local guarda a correção no commit `92f2b57`; o patch é a forma
versionada no projeto principal. A reprodução histórica permanece em
`patches/pending/` apenas como registro.

## REA e Ghidra

O Ghidra conectado confirmou `SLUS_210.67`, linguagem `r5900:LE:32:default`,
SHA-256 `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de `0x3cc040` identifica a verificação de alcance dos saltos Lua;
a decompilação de `0x252948` confirma o acesso ao vetor de opções em `this+0x24`
e a seleção em `this+0x18`. Estas são observações estáticas.

REA 6.8.0 foi usado pela CLI e por uma sessão MCP temporária, sem alterar a
configuração do cliente. Evidências locais:

- Identidade do ELF: `ev_5b0057281986d06a5eb8b00fdccaf348cf31f05ebb819f58f979af3a668c83d9`,
  `/tmp/dra2-rea-elf-20261107.json`. GNU objdump identifica o contêiner e entry,
  mas não reconhece a arquitetura R5900; a arquitetura foi confirmada no Ghidra.
- Anexação bloqueada pelo Yama/ptrace: `ev_fc1efaa50d91f5a816779e89e97182c0a5a2a7e37b0de15ba78a021a9538c55a`.
  Nenhuma RAM foi capturada por essa tentativa.
- Execução do binário de regressão sob REA/GDB:
  `ev_7a89de57e5dd4974aa6ea6242998c21144b92526f3fa2b7195b59858ddd356d3`,
  `/tmp/dra2-rea-checkpoint-v2-tests-output.json`; o inferior saiu normalmente.
  As contagens dos testes vêm dos logs de execução direta, não desse registro.

## Validação no jogo

A captura `.cache/menu-v2-ram.bin` confirma que `lua/menu` agora compila:
existe a global `menu`, a tabela `menu.main.carousel` contém `carouselCount=5`
e os cinco itens. A sonda na rotina original `0x252948` observou o objeto
`0x01a52a80`, estado 2, seleção 1, vetor C++ com cinco entradas. A RAM foi
lida com o mesmo parser de estruturas Lua usado na sessão anterior.

A execução final `.cache/menu-v4-run.log` confirmou:

1. Estado 2, seleção 1, cinco entradas no vetor do menu.
2. Transição para estado 6 (interativo).
3. Seta direita real, após foco por endereço da janela no Hyprland.
4. Mudança de seleção de 1 para 2, com reinício da animação (estado 0).

O comando `python3 /tmp/dra2-boot-menu.py /tmp/dra2-checkpoint-v2-menu`
terminou com código 0 e `MENU VALIDADO: cinco opções; seleções {'2', '1'}`.
O harness usado nesta sessão depende da sonda temporária e está fora do git.

A imagem `fb000_t07440.ppm` mostra a opção Multiplayer no framebuffer de
apresentação FBP 0. `fb000_t08040.ppm` mostra o logotipo completo após a seta.
Conversões locais: `.cache/menu-inicial-opcoes.png` e
`.cache/menu-inicial-validado.png`. O FBP 70 contém o fundo intermediário:
usá-lo sozinho ocultava a interface efetivamente composta em FBP 0.

**O objetivo de chegar ao menu inicial navegável foi atingido.** Há defeitos
visuais em alguns frames de transição: partes do logotipo e dos elementos do
carrossel ficam pretas; a captura final apresenta o logotipo completo.
A navegação foi validada entre duas opções; entrar em uma partida não foi testado.

As sondas somente de leitura foram retiradas dos overrides, e `build/dra2`
foi recompilado e ligado novamente sem elas. O override permaneceu idêntico
ao início da sessão. O binário instrumentado já carregado foi usado para a
última navegação; o executável final em disco contém apenas a correção permanente.
Os testes e a aplicação sequencial dos patches haviam passado antes da limpeza.
Capturas incompletas por falta de espaço em `/tmp` foram descartadas como evidência.
RAM, imagens e recursos permanecem em caminhos ignorados.

## Próximo passo

Investigar o desenho dos elementos do carrossel durante suas animações e
validar a entrada de uma opção. Preservar a compilação Lua e o comportamento
do dispatcher; não restaurar a investigação do erro Lua já corrigido.

O foco de Xwayland pode ser perdido durante aprovações/foco do terminal.
O harness final selecionou a janela pelo PID em `hyprctl -j clients` e executou
`hyprctl eval 'hl.dispatch(hl.dsp.focus({window="address:..."}))'` imediatamente
antes das teclas. Nenhuma configuração do desktop foi alterada.
Fonte: [API atual do hyprctl](https://wiki.hypr.land/configuring/core/advanced-configuration/using-hyprctl/).

Referência primária consultada nesta sessão:
[arquitetura e fluxo de diagnóstico do PS2Recomp](https://github.com/ran-j/PS2Recomp).
