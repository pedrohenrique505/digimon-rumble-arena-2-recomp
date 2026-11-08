# paraLLEl-GS: início da integração ao jogo (etapa 3)

Retomada do fluxo de importação do paraLLEl-GS a partir da prova offline.
O jogo ainda usa o GS CPU. Esta entrada registra a investigação com
Ghidra/REA que dimensiona os dois bloqueadores restantes do replay antes
do adaptador no jogo: leitura FIFO local→host e upload IMAGE parcial.

## Identidade (Ghidra + REA)

Ghidra (GUI, projeto do usuário) confirma `SLUS_210.67`, MIPS R5900 LE,
SHA-256 `590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
REA 6.8.0 via CLI `inspect-native-exports` reproduziu o mesmo artefato:
Evidence ID `ev_5123ea7e012701c3bf310d62ee24731816d4785ff6a07accd0793deca81b021c`,
ELF stripped sem tabela dinâmica de símbolos. É a mesma evidência da
sessão anterior; nenhuma alegação nova de execução decorre dela.

## sceGsExecStoreImage no Ghidra

As strings de erro `sceGsExecStoreImage: ...` em `0x00516d60` têm xref
único de `FUN_003f0498`. A decompilação mostra a rotina de libgraph:

- calcula o total em bytes por PSM (`RRW*RRH*bpp/8`) e separa qwords
  inteiros, resto de bytes (`total % 16`) e qwords excedentes a descartar;
- lê `REG_VIF1_FIFO` em qwords de 16 bytes e copia a cauda parcial
  byte a byte para o destino.

A função foi renomeada para `sceGsExecStoreImage` no projeto Ghidra do
usuário e recebeu comentário com essa semântica. É observação estática.

Consequência para o adaptador: o consumidor pede `totalImageBytes`
arbitrário (no runtime, `sceGsExecStoreImage` HLE chama
`consumeLocalToHostBytes(dst, totalImageBytes)`), enquanto o candidato
só entrega qwords de 16 bytes e preenche zeros após o fim. O adaptador
precisa de um buffer residual que preserve bytes não consumidos entre
chamadas, sem arredondar o pedido. A cauda sub-qword da transferência
(`required_bytes % 16`) nem entra no FIFO do candidato
(`fifo_readback_128b_size = required_bytes / 16` na revisão pinada) e
precisa ser reconstruída a partir da VRAM sincronizada.

O mesmo truncamento por 64 bits afeta uploads IMAGE no fluxo GIF geral:
`required_qwords = (RRW * RRH * bpp) / 64` descarta o último word parcial
(ex.: 1 pixel CT32 ou 3 pixels CT24), como já corrigido para uploads
nativos no replay. Falta a cobertura do caminho GIF IMAGE (tipo 1).

Fontes primárias: [API pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_interface.hpp)
e [implementação pinada](https://github.com/Arntzen-software/parallel-gs/blob/cc6184af7e0c03da603045ca371ffa5dae9b0655/gs/gs_interface.cpp)
(`init_transfer`, `read_transfer_fifo`, `a_d_TRXDIR`).

## Sequência desta rodada

1. Adaptador FIFO local→host no replay com preservação de consumo
   parcial e reconstrução da cauda sub-qword, com testes.
2. Upload IMAGE (GIF tipo 1) com word parcial via fallback CPU sobre
   VRAM sincronizada, com testes.
3. Atualizar `NEXTSTEPS.md`/`docs/gs-acelerado.md` e só então iniciar o
   adaptador GIF no jogo (etapa 3), em commits pequenos.

Nenhum dado do jogo foi adicionado ao git.
