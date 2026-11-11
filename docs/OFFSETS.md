# Kernel offsets used by the runtime modules (verified on the target)

Every offset below was observed on the running vendor `Image` (5.10.236) with a
QEMU probe, not copied from another kernel. The probe (`scprobe`) kprobes
`shrink_node` and prints the bytes of `struct scan_control` at entry.

## `struct scan_control` (private to `mm/vmscan.c`)

| field | offset | observed |
|---|---:|---|
| `nr_to_reclaim` | `0x00` | 2047 (kswapd) / 32 (direct) |
| bitfields (`may_deactivate:2`, `force_deactivate:1`, `skipped_deactivate:1`, …) | `0x28` | 0 |
| `order` | `0x2a` | 0 |
| **`priority`** | **`0x2b`** | **12 → 11 → 10 → 9** (DEF_PRIORITY decreasing) |
| `reclaim_idx` | `0x2c` | 0 / 2 (zone index) |
| `gfp_mask` | `0x30` | `0xcc0` / `0x3100dca` |
| `nr_scanned` | `0x38` | 0 at `shrink_node` entry |
| `nr_reclaimed` | `0x40` | 0 → 11 during reclaim |

**Correction:** an earlier revision of `mglru_probe` used `0x2c` for
`priority`. The probe shows `0x2c` is `reclaim_idx`; `priority` is `0x2b`.
`mglru_probe` 1.0.6 and `vmscan_hook` use `0x2b`. This is a real defect that
would have made the "priority policy" write the reclaim zone index instead.

Why the layout looks unusual: the 13 bitfields are packed into two bytes by
clang, so `order/priority/reclaim_idx` start at `0x2a`, not `0x2c`.

## `struct lruvec`

| field | offset | source |
|---|---:|---|
| `nonresident_age` | `0x60` | `include/linux/mmzone.h:290` (lists 0x00–0x4f, anon_cost 0x50, file_cost 0x58) |
| `refaults[FILE]` | `0x70` | `mmzone.h:292` |
| `flags` | `0x78` | `mmzone.h:294` |
| `pgdat` | `0x80` | `mmzone.h:296` |

(Observed by the module readback: `lruvec.size=136`, `flags=120`, `pgdat=128`.)

## Workingset shadow

`workingset_eviction` packs `nonresident_age` into the shadow; on this kernel
`eviction = shadow >> 19` (`xa_to_value >>2`, workingset bit `>>1`,
`NODES_SHIFT=0`, `MEM_CGROUP_ID_SHIFT=16`, `bucket_order=0`). Refault distance =
`(nonresident_age_now - eviction) & ((1<<46)-1)`.

## How to re-verify

`modules/scprobe` is a diagnostic module (not part of the release): load it,
run reclaim, read `dmesg | grep scprobe`. It prints `o2a p2b r2c x2d scan38
recl40 to0 gfp30`. A change in any value invalidates the offsets above and the
modules must be rebuilt for the new target.
