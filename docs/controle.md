# Controle no Linux

O runtime aceita controle físico reconhecido pelo raylib e teclado no port 1.
O teclado funciona mesmo em modo analógico. A janela do jogo precisa ter foco.

| PS2 | Teclado |
| --- | --- |
| Direcional | Setas |
| Analógico esquerdo | W A S D |
| Cruz / Círculo / Quadrado / Triângulo | X / C / Z / V |
| Start / Select | Enter / Shift direito |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| L3 / R3 | Ctrl esquerdo / Ctrl direito |

Mapeamento efetivo dos bindings SDK em
`external/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Pad.cpp`.
Pressione e solte Enter para avançar pelo aviso de autosave.
Não é necessário configurar arquivos do sistema.
