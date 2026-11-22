# Roteiro de controle até o menu

`menu-controle.txt` é a sequência gravada do usuário; `menu-controle-rapido.txt`
limita cada pausa entre mudanças de estado a cinco segundos, preservando os
apertos. O usuário confirmou que a versão rápida alcançou o menu nesta máquina.
As esperas são relativas à primeira leitura do controle e usam tempo de parede:
carregamentos ou desempenho diferentes podem fazer um comando chegar cedo demais.
Confirme a tela alcançada antes de interpretar uma medição.

O patch 0026 adiciona captura e reprodução no controle do jogo (teclado ou
controle físico), sem capturar teclas de outros aplicativos. Cada linha contém
segundos, máscara de botões ativa em zero e quatro eixos analógicos. O arquivo
de gravação deve ser novo. Os valores de replay substituem a entrada física.

```bash
cd game
DRA2_ISO='/caminho/para/disco.iso' DRA2_GS_BACKEND=gpu-present \
  DRA2_PAD_REPLAY="$PWD/../tools/scenarios/menu-controle-rapido.txt" \
  ../build/dra2 SLUS_210.67
# Para gravar outra sessão, use DRA2_PAD_RECORD=/caminho/novo.txt no lugar de DRA2_PAD_REPLAY.
```

O replay não verifica automaticamente a tela ou a seleção e não é um teste de
correção visual. A imagem do carrossel enviado pelo usuário mostra artefatos;
reproduzir a sequência não os corrige.
