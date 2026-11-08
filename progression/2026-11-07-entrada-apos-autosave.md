# 2026-11-07: entrada real após o aviso de autosave

Verificado o mapeamento efetivo em Kernel/Stubs/Pad.cpp. A entrada do
teclado é combinada com a do gamepad. Não havia dispositivo em
/dev/input/by-id nesta sessão; a validação foi pelo teclado.

Executado build/dra2 com a ISO e DRA2_DUMP_FB=/tmp/dra2-menu.
A janela PS2-Recomp foi focada com xdotool e recebeu press/release de
X e Enter (1–2 s). O aviso ainda estava presente no tick 480 após a
primeira tentativa de X; depois de Enter, desapareceu no tick 1200.
Isso confirma resposta à entrada, sem injetar botões no HLE nem alterar
estado do jogo. A correção anterior de libpad anuncia DualShock ao jogo.

Enter e X adicionais não produziram menu visível até o tick 2880.
FBP 0 permaneceu preto com uma linha verde inferior; FBP 70 tick 2400
estava inteiramente preto. FBP 280 no mesmo tick tinha três cores e
128.742 pixels não pretos. Portanto não há evidência de menu inicial
renderizado; a entrada avançou o aviso, mas a etapa seguinte continua
com problema visual ou de progressão ainda não discriminado.

Mapeamento registrado em docs/controle.md. Nenhuma mudança no runtime
foi necessária para receber teclado. Próxima investigação: identificar
estado do jogo após o aviso e comparar os pacotes/targets GS com referência.
Capturas e log ficam em /tmp/dra2-menu e /tmp/dra2-menu.log.
