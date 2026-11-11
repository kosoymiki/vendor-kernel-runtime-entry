# MGLRU Stage F — M4 parity subset: per-memcg scan-epoch clock

Stage F replaces the global 64-scan generation counter with a per-memcg
**scan-epoch clock** derived from the vendor reclaim tracepoint, and scales the
refault threshold by the generation window. It is the runtime parity subset for
M4; a vmscan rebuild is not required.

## Mechanism

- The vendor tracepoint `android_vh_shrink_node_memcgs` fires for each memcg
  scan. `tp_memcg_probe` advances that memcg's `seq` and sets
  `window = min(seq, 2)` — the analogue of `max_seq - min_seq`.
- `mark_page_accessed` stores the page's `seq` in the folio shadow table.
- `workingset_eviction` compares the page generation with the current memcg
  `seq`: equal means **young** (accessed in the current epoch), otherwise
  **cold**.
- `get_scan_count` policy:
  1. refault distance `d`: `d <= 32 * window` zeroes file scans, else halves;
  2. young working set > 60%: halve file scans;
  3. cold pages < 20% young: keep the full scan;
  4. otherwise per-memcg generation parity;
  5. `can_swap` last, only zeroing anon counts.
- Readback adds `seq=` and `win=`; the policy is still debug-injectable
  (`t5` distance, `y80` young/cold).

## Observed in QEMU (stock `Image`, 512 MiB, swapless)

`tests/qemu_ktramp_gate.sh`, module sha `34bf93d0…`:

| regime | counters | branch | result |
|---|---|---|---|
| cold ext4 cache | `seq=125`, `win=2`, `nr_calls=125` | 7 (full scan) | `delta20=2 / pre20=608` |
| forced young `y80` | `seq=133`, `win=2` | 6 | `delta20=591 / pre20=1167` = **50.64%** |
| forced distance `t5` | `seq=25`, `win=2` | 1 | `delta20=8703 / pre20=8703` = **100%** |
| real refault | `seq=141`, `win=2` | 2 | `delta20=376 / pre20=735`, `tdist=847` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

The clock advances with real reclaim (`seq` 25 → 141 across regimes) and the
window saturates at 2, matching the intended two-generation window.

## Tests

`modules/ktramp/tests/test_policy.c`: 31/31 PASS, including the window-scaled
threshold (`d=40 <= 64` zeroes file, `d=70 > 64` halves).

## Limits

- The epoch is advanced per memcg scan (vendor tracepoint), not at the exact
  MGLRU point where a new generation is created; per-generation LRU lists are
  still side tables, not `lru_gen` structures.
- The window is a fixed two epochs; adaptive `min_ttl` is M5 work.
- The policy changes `nr[]` only; kernel data structures are not modified.
