# Segunda divergência localizada na captura nova

Continuação da integração paraLLEl-GS, seguindo a retomada da sessão anterior.
O CPU continua autoritativo; GPU continua shadow. Não houve alteração de timing,
correção do rasterizador, aceite visual ou apresentação na janela nesta etapa.

## Ferramenta e reprodução

`tools/find_gs_first_difference.py` transforma a varredura local da sessão
anterior em ferramenta versionada. Compara prefixos completos sequencialmente,
trata registros 3/4 de upload como unidade e preserva relatório, prefixos e VRAMs
em diretório novo. Não usa busca binária: diferenças podem desaparecer depois.

Comando executado:

```bash
python3 tools/find_gs_first_difference.py \
  .cache/performance/parallelgs-constant-fixed/capture/complete.gs \
  .cache/performance/parallelgs-prefix-tool
```

Captura SHA-256:
`9c46301a57200e850f0e11802aa8692844df455a19dc19965c6d170976ebb2d5`.
O primeiro registro divergente é **38**, GIF de 2.096 bytes. Até o registro 37,
os 4 MiB são idênticos. No registro 38 divergem 1.344 bytes, nos offsets físicos
497.096 a 573.439 (não necessariamente contíguos). Há 672 pares CPU/GPU 240/0
e 672 pares 95/0. O prefixo `after.gs` tem SHA-256
`898631d792e4f29e060d013f550b5d7759a944427fdd2db74ed942c754339544`.
Uma execução anterior do probe local encontrou exatamente o mesmo resultado.

## Redução e limites

Scripts e resultados exploratórios estão em
`.cache/performance/parallelgs-next-diff/`, fora do git.
Redução por remoção, preservando a presença do par 240/0, reteve registros
0, 22, 32, 36 e 38. Dentro do pacote final, quatro escritas de configuração e
um par de vértices de sprite ainda reproduzem esse par. Essa redução também
introduz outras diferenças: **não** é uma reprodução isolada e equivalente
de todos os 1.344 bytes do prefixo original. Não usar seu total como métrica
de melhora nem converter diretamente esse pacote em correção.

Hipóteses abertas: amostragem de textura, coerência entre desenho e textura,
e semântica dos testes/formato de framebuffer. Probes no prefixo original,
cada qual variando um estado, ainda não distinguem uma causa única.
Desativar o filtro via TEX1 mantém o padrão original; mudar TEST para
0x30000 reduz a contagem, mas deixa diferenças. Mudar coordenadas de textura
introduz outros pares. Nenhum desses experimentos justifica um patch.
Os comandos modificados são apenas diagnósticos, não substituem a captura.

Uma segunda redução preservou todos os registros anteriores e as nove
escritas de configuração do pacote 38, removendo somente pares de vértices.
O primeiro sprite sozinho reproduz **64 bytes**, 32 pares 240/0 e 32 pares
95/0, sem outros pares divergentes, a partir do mesmo estado igual até 37.
Artefato: `equal-state-sprite.gs`; script: `equal-state-reduce.py`. Isso isola
uma ocorrência do padrão original, sem demonstrar ainda sua causa.

## REA, Ghidra e fonte consultada

REA inspecionou `GSCpuBackend::DrawSprite` no replay CPU com GNU objdump,
sem abrir sessão nativa. Evidence completo salvo em `rea-sprite.json` no
diretório local acima. Pai:
`ev_8ac49c97e446f8dd5c55045bf27f94786683b7b6d5505770a8e7ff5f1290e04e`.
Binário SHA-256:
`96e32869a6b95da0673a495c2b9c893b9e2c299377761ea17e6e5536082b16ce`.
Operação completa, sem truncamento nem mudança do artefato. O assembly mostra
o caminho texturizado, interpolação e chamadas a SampleTexture e WritePixel.
Isso confirma o código compilado, não uma execução específica do pacote 38.

Ghidra confirmou SLUS_210.67, SHA-256
`590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd`.
A decompilação de FUN_003a2460 confirma escritas em ambos os circuitos de
exibição e `uGp00000428 ^= 1`. Não identifica o produtor do pacote 38.
A sessão GUI existente foi preservada.

Fonte primária consultada antes de tentar uma correção:
[PCSX2 GSRendererSW.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/SW/GSRendererSW.cpp).
Snapshot SHA-256:
`61016efbbd3b3ef60b9df9f9fc5b658f16d30ab525d592f256699a1c357b3570`.
Não foi obtida referência de execução do hardware original.

## Validação e retomada

A ferramenta reproduziu a fronteira 37/38 em aquisição real. Os 16 casos de
captura/replay CPU e os 16 Vulkan passaram. Artefatos do jogo continuam
ignorados pelo git. Não foi necessário rebuild: só ferramenta Python e docs
foram alteradas.

Próximo passo: usar o sprite isolado com estado anterior igual para localizar
os pixels/texels dos 64 bytes e distinguir amostragem, coerência e formato.
Depois, criar teste sintético independente do jogo que distinga a hipótese
confirmada antes de corrigir o runtime. O aceite visual e a apresentação GPU
na janela continuam pendentes; a integração completa não está concluída.
