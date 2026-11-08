# Reproduções pendentes

Estes patches registram reproduções e investigações históricas e não são aplicados por
`tools/apply_upstream_patches.sh`.

- `0013-recursive-checkpoint-regression.patch`: dois testes do dispatcher.
  Reprodução histórica. Corrigida pelo patch aplicado
  [0013-propagate-recursive-checkpoint-yields.patch](../ps2recomp/0013-propagate-recursive-checkpoint-yields.patch), que inclui
  estes dois testes e um terceiro para yield no PC de continuação do caller.

O branch local `dra2-recursive-yield-repro` em `external/PS2Recomp` conserva
a reprodução. Consulte
[`progression/2026-11-07-compilador-lua-e-checkpoint.md`](../../progression/2026-11-07-compilador-lua-e-checkpoint.md)
antes de retomar a correção.

Correção e validação no jogo: [menu inicial navegável](../../progression/2026-11-07-checkpoint-recursivo-corrigido.md).
