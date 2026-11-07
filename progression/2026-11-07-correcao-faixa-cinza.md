# 2026-11-07: correção da faixa cinza em MIN/MAX do VU1

## Causa e correção

O interpretador normalizava todos os operandos das instruções superiores VU,
apagando denormais. MIN/MAX devem selecionar os bits originais dos operandos.
O jogo usa essas instruções para copiar atributos gráficos: na captura do
microcódigo, MAXz em PC 0x1ee8 copia 0x58 do qword 0xde.z; SQ.x em PC 0x1f08
escreve o resultado no PRMODE. A normalização transformava esse valor em zero,
desligando a textura e produzindo retângulos cinza no lugar das letras.
A sonda anterior à correção está em /tmp/dra2-vu-store.log.

O patch 0009 preserva operandos nas variantes vetorial, broadcast e immediate,
usa comparação inteira com ordem invertida entre negativos e mantém I bruto
até o consumidor. Operações aritméticas continuam normalizando seus operandos.
As sondas desta investigação foram removidas; alterações locais anteriores
foram preservadas.

Fonte primária: [PCSX2 VUops.cpp, fp_max/fp_min](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VUops.cpp).
A análise do binário pelo Ghidra confirmou SLUS_210.67, R5900,
SHA-256 590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd.
Não há Evidence IDs REA: o diagnóstico REA 6.8.0 identificou registro ausente
no perfil Codex; a investigação prosseguiu pelo Ghidra conectado e sondas locais,
sem alterar configuração do cliente.

## Validação

- Build dra2 concluído.
- Suíte PS2VU1: 41 testes passaram, zero falhas (/tmp/dra2-vu-green.log).
- Patch validado com git apply --reverse --check.
- Execução normal com DRA2_DUMP_FB=/tmp/dra2-minmax-fixed, sem forçar TME.
- FBP 70 tick 480: 143 cores, 24.574 pixels não pretos; detector de faixa cinza passou.
- Inspeção visual de validated.png confirmou o aviso de autosave completo,
  com texto legível e “Press any button to continue”.

A faixa cinza foi corrigida nesta tela. Não houve validação de menus ou gameplay;
esses estados ainda precisam de comparação visual própria. Dumps permanecem
em /tmp e nenhum conteúdo do jogo foi versionado.
