# Reproduções pendentes

Estes patches registram investigações incompletas e não são aplicados por
`tools/apply_upstream_patches.sh`.

- `0013-recursive-checkpoint-regression.patch`: dois testes do dispatcher.
  No runtime atual, o caso de yield recursivo falha; o caso de retorno HLE com
  evento postado passa. Ainda não há correção implementada.

O branch local `dra2-recursive-yield-repro` em `external/PS2Recomp` conserva
a reprodução. Consulte
[`progression/2026-11-07-compilador-lua-e-checkpoint.md`](../../progression/2026-11-07-compilador-lua-e-checkpoint.md)
antes de retomar a correção.
