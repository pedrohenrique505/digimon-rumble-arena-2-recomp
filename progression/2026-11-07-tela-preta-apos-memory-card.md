# 2026-11-07: tela preta depois do aviso de autosave

## Reprodução e evidência

`python3 /tmp/dra2-repro-black.py /tmp/dra2-black-baseline` inicia o executável
com a ISO, espera a captura do tick 480 e envia press/release de Enter pela
janela real. A execução original falhou: todos os seis dumps do FBP 70 entre
os ticks 1200 e 1800 tinham zero pixels não pretos. O aviso inicial aparece
antes da entrada; o jogo continua enviando primitivas depois dela.

O Ghidra conectado confirmou `SLUS_210.67`, R5900, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
REA 6.8.0 doctor constatou registro ausente no perfil Codex. Não foi alterada
configuração do cliente; as evidências abaixo são do Ghidra e de sondas locais,
sem Evidence IDs REA.

## Causa rastreada

1. Os triângulos da fonte herdavam TEX0 da cópia do framebuffer (`TBP0=2240`),
   em vez de `TEX0=0x20079c0621313be0` da fonte.
2. O pacote XGKICK 3016 continha o valor correto de TEX0, mas o endereço A_D
   era zero em vez de seis. O template persistente em VU qword `0x2b2` estava
   corrompido, antes do parser GIF e do GS.
3. Uma sonda de UNPACK identificou a primeira mudança em `0x2b4.z`: de seis
   para zero, causada por interpretar `0xffffffff` como comando VIF.
4. No fluxo capturado, UNPACK V3_32 NUM=30 começa em `0x8428`. Seus 360 bytes
   terminam em `0x8594`; há oito bytes `ff` antes do V4_32 em `0x859c`.
   Esses bytes são comandos válidos, não padding que o VIF possa ignorar.
5. O DMA apenas copia esse conteúdo: o fluxo vem de um payload CNT, sem TTE.
   Um watchpoint na RAM EE `0x603324` encontrou a cópia por SPR_FROM, disparada
   por `FUN_00427578`. Não é reordenação entre os paths GIF.
6. O produtor `FUN_00428e80`, no PC EE `0x429150`, executa
   `SQ $zero,0($t3)` com endereço terminado em `c`. No R5900, SQ descarta os
   quatro bits inferiores e limpa o qword que contém o padding. A tradução
   usava o endereço sem alinhar. No scratchpad, o acesso lento gerava exceção;
   na RAM, o helper rápido escrevia a partir do endereço exato.

A rotina VU de material `0x1e80–0x1f20` só atualiza x/xy dos slots A_D;
portanto não recupera os endereços z corrompidos. Isso explica a persistência
até os frames seguintes, mesmo com vértices e valores de textura corretos.

Fontes primárias:

- [PCSX2 R5900OpcodeImpl.cpp, LQ/SQ](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/R5900OpcodeImpl.cpp): ambos mascaram `addr & ~0xf`.
- [PCSX2 Vif_Unpack.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif_Unpack.cpp): V3_32 consome 12 bytes por vetor, com arredondamento a word.
- [PCSX2 VU0.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU0.cpp): LQC2/SQC2 passam o endereço sem esse mascaramento.

## Escopo da correção

Corrigir no tradutor de LQ/SQ, incluindo endereços constantes resolvidos por
MemoryAccessHint. Preservar os helpers de memória compartilhados com COP2.
Não ignorar comandos `0xffffffff` no VIF, nem forçar textura ou estado do jogo.

As sondas foram retiradas, preservando as alterações locais anteriores em
GS, VIF, VU e contadores. Capturas de RAM, pacotes, código derivado e logs
continuam fora do git, em `/tmp` e diretórios ignorados.
