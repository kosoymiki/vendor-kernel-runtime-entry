# MGLRU Stage G — adaptive ttl and eviction-age branches (M5)

Stage G adds two runtime analogues that were still missing from the M4 subset:

- **adaptive ttl** — the refault window is no longer fixed at 2. A per-memcg
  `ttl` in `[2,4]` is raised when refaults dominate evictions (thrashing) and
  lowered when they do not (`kt_ttl_step`). The refault threshold becomes
  `32 * ttl`.
- **eviction-age histogram** — every eviction is counted in a 4-bucket age
  histogram (`0`, `1`, `2`, `3+` epochs). The policy uses it after the binary
  young/cold check: old evictions (>70%) keep the full scan (branch 8), young
  evictions (<30% old) halve file scans (branch 9).

## Mechanism

- `tp_memcg_probe` advances `seq`, sets `window=min(seq,2)` and updates
  `ttl = kt_ttl_step(evictions, refaults, ttl)`.
- `workingset_eviction` increments `ev_age[memcg][bucket]` from
  `seq - page_seq`.
- `get_scan_count` policy order: refault distance (`32*ttl`) → young/cold →
  eviction-age 8/9 → generation parity → `can_swap` last.
- Readback adds `ttl=` and `age=a,b,c,d`.

## Observed in QEMU (stock `Image`, 512 MiB, swapless)

`tests/qemu_ktramp_gate.sh`, module sha `e688ddc3…`:

| regime | counters | branch | result |
|---|---|---|---|
| cold ext4 cache | `seq=123`, `ttl=2`, `age=0,0,0,6362` | 7 (full scan) | `delta20=1/610` |
| forced young `y80` | `seq=133`, `ttl=2` | 6 | `delta20=376/737` = **51.0%** |
| forced distance `t5` | `fd=5` | 1 | `delta20=3070/3070` = **100%** |
| forced old evictions `a80` | `fa=80/100` | **8** (full scan) | `branch8=59` |
| forced young evictions `a10` | `fa=10/100` | **9** (protect) | `delta20=375/734` = **51.1%** |
| real refault | `seq=118`, `ttl=2` | 2 | `tdist=848` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

The age histogram is populated by real reclaim (`age=0,0,0,6362` on the cold
cache; `age=1,0,0,6247` after the refault phase), and the ttl step keeps the
window at 2 because the refault/eviction ratio stays below the 50% threshold in
this workload. The ttl change itself is unit-tested (`kt_ttl_step`: 60/100 →
3, 5/100 → 3 from 4, 20/100 → 2, saturation at 4).

## Tests

`modules/ktramp/tests/test_policy.c`: **38/38 PASS** — branches 1–9, window
threshold, ttl step, no-swap ordering, shadow unpack, folio head, eviction
table.

## Limits

- `ttl` is a generation window, not `min_ttl_ms`; it is driven by the
  refault/eviction ratio, not wall-clock time.
- The age histogram counts evictions, not resident pages; it is a side-table
  approximation of per-generation list sizes.
- Branch 8/9 are proven with labeled injections (`a80`/`a10`); the cold and
  refault workloads reach branches 7/2 with real state.
