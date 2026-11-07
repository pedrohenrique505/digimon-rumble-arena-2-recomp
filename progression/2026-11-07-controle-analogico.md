# 2026-11-07: erro de controle por tabela de modos incompleta

## Commit e investigação da faixa

O marco anterior foi commitado como `414abf9` (inspeção de dumps e evidência
de que a faixa já está no FBP 70).

Uma sonda temporária capturou os primeiros triângulos não pretos no FBP 70:
cor RGB(128,128,128), STQ variando por caractere, Q=1, textura em TBP0=15328,
PSM=19 (PSMT8), CLUT em CBP=15584. Estado efetivo: `PRMODECONT=0`,
`PRMODE.TME=0`, `PRIM.TME=0`.

O pacote bruto recebido pelo GS contém uma GIFtag PACKED de nove escritas
A+D, seguida dos triângulos. No offset 0x80 está o par valor=0, registrador=0x1B
(PRMODE). Portanto, a desativação da textura já está no pacote recebido;
não é apenas uma interpretação incorreta desse bit no rasterizador.

Captura de pilha com GDB localizou o caminho:
`FUN_003a2460 -> sceDmaSend -> processVIF1Data -> VU1Interpreter::run ->
progressXgkick -> submitGifPacket(PATH1) -> GS::processGIFPacket`.
O Ghidra confirmou que `FUN_003a2460` finaliza/submete os buffers do frame.

## Experimento de visibilidade, não uma correção

Forçar temporariamente TME **somente nos triângulos da fonte** permitiu ler:

> The analog controller is not detected. Please connect an analog controller
> to controller port 1, and press any button to continue.

O primeiro dump desse experimento era preto; um dump posterior continha a
mensagem completa. Isso reforça a limitação das capturas por tick no meio
do desenho. Não concluir falha de upload só porque um snapshot ficou preto.

A mensagem explica por que a execução anterior não prosseguia para a tela
inicial. O experimento também demonstra que a textura da fonte pode produzir
glifos legíveis quando amostrada; não valida o pipeline de texturas inteiro.
Depois da correção de controle, a página da fonte muda, então o experimento
de leitura seguinte selecionou triângulos PSMT8, sem fixar TBP0.

Todas as sondas `[DEBUG-dra2-attrs]`, dumps de pacotes e a opção temporária
`DRA2_PROBE_FONT_TME` foram removidos do código. O executável final foi
reconstruído sem forçar textura. As alterações de diagnóstico que já existiam
antes desta sessão no clone local foram preservadas.

## Causa confirmada em libpad

No Ghidra, `FUN_003edbb8` enumera `scePadInfoMode(port, slot, 4, -1)` e cada
entrada da tabela, procurando o tipo 7 (DualShock). Só depois chama
`scePadSetMainMode` com o índice encontrado. Se não encontra 7, marca o
controle como incompatível e apresenta a mensagem acima.

O HLE retornava uma tabela de uma entrada contendo **o modo atual**. Como o
controle abre em digital, essa entrada era 4 e o jogo nunca tentava ativar
o analógico. A tabela precisa anunciar capacidades, independentemente do
modo atual.

Patch `0008`, commit local do runtime `dc9010d`:

- Tabela fixa: índice 0 = digital (4), índice 1 = DualShock (7), tamanho 2.
- `PAD_MODECUROFFS` acompanha o modo atual: 0 digital, 1 analógico.
- O controle continua abrindo em digital, com a seleção feita pelo jogo.
- Regressão cobre enumeração antes da seleção, ativação pelo índice encontrado,
  ID analógico no Read, tabela estável e índices inválidos/controle fechado.
- Corrigida uma expectativa antiga do teste que confundia tabela com modo atual.

Fonte primária consultada:
[ps2sdk libpad.c, padInfoMode](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/pad/src/libpad.c).
O SDK separa `nrOfModes`/`modeTable` de `modeCurId`/`modeCurOffs`.
Para a seleção de atributos gráficos, também foi consultado
[PS2Tek, PRMODECONT/PRMODE](https://psi-rockin.github.io/ps2tek/).

## Validação

- Suíte existente `PadInput`, compilada com um runner temporário que registra
  apenas `register_pad_input_tests()` e liga o runtime real do build local.
  Inclui `test_function_table.cpp` do upstream para a tabela de funções de teste.
- Antes da correção: 13 testes, 11 passaram, 2 falharam (nova regressão e
  expectativa de enumeração atualizada); log `/tmp/dra2-pad-red.log`.
- Depois: **13 passaram, 0 falharam**; log `/tmp/dra2-pad-green.log`.
- Build `cmake --build build --target dra2 -j5` concluído.
- Execução normal de 22 s, sem forçar TME, estável; dumps agora mostram
  outra mensagem, mas ainda sem textura correta.
- Com a sonda temporária de leitura, o FBP 0 no tick 480 mostrou o aviso
  **“Digimon Rumble Arena 2 uses an Autosave feature…”**, com “Press any button
  to continue”. Isso confirma que o jogo saiu do bloqueio de controle analógico.
  A mensagem foi lida em `/tmp/dra2-pad-fixed-text/fb000_t00480.ppm`.
- O bug gráfico permanece: a execução normal ainda desenha blocos cinza.
  Não houve validação de menus ou gameplay nesta etapa.

## Próximos passos

1. Rastrear a formação do campo PRMODE no microcódigo VU1 e nos dados que
   chegam por VIF. Não forçar TME em definitivo: limpezas e geometria sem
   textura também usam esse pipeline.
2. Após corrigir a origem dos atributos, testar o avanço do aviso de autosave
   com entrada real, chegando aos menus.

Artefatos de diagnóstico e imagens continuam apenas em `/tmp`; nenhum
conteúdo do jogo foi adicionado ao git. A correção genérica de controle e
seus testes ficam versionados no patch `0008`, sem PR upstream.
