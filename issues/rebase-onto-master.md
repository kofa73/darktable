# gradient-hl lacks master's fix for -t below the CPU count (#22436)

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending. Kind: bug (branch state).

## Problem

`darktable-cli -t N` with N below the CPU count overflows per-thread buffers on this branch:
GraphicsMagick's init resets the OpenMP thread count after `dt_init()` sets it. Fixed on master
(#22436, related #22567); `gradient-hl` predates the fix. Seen as heap corruption in
`dt_box_mean()` with segmentation based and rebuild (2026-10-21 log).

## Workaround

Set `OMP_NUM_THREADS` to the `-t` value (all runners under `/tmp/hl-rules-study/runs/` do).

## To do

Rebase `gradient-hl` onto master when Kofa decides; only Kofa commits or rebases.
