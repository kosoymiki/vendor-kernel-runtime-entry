# mglru_probe — reclaim-path probe with a priority policy (experimental)

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

This is a **probe**, not full MGLRU. It registers kprobes on the 5.10 reclaim
path, exposes counters through `/proc/mglru_probe`, and can apply a policy at
`shrink_node` entry.

## What it does

- **Stage A (read-only):** kprobes on `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`.
- **Stage B (policy):** write `1` anon-light, `2` file-light, `3` aggressive,
  `4` generation-aware, `5` priority policy to `/proc/mglru_probe`.
- `mark_page_accessed` kprobe feeds a 1024-entry page-generation side table.
- Mode 5 writes `scan_control.priority` (offset `0x2c`) to `0` at `shrink_node`
  entry and counts entries (`phits`) and real changes (`changed`).

## Observed in QEMU (exact stock Image)

Gate: `tests/qemu_mglru_gate.sh` (ext4 file through `loop0` + 345 MiB anonymous
pressure, poll `pgscan_file`; see `docs/MEASURING-RECLAIM.md`).

| metric | mode 0 | mode 5 |
|---|---|---|
| `get_scan_count` hits | 101 | 102 |
| `page_referenced` hits | 5266 | 4436 |
| `pgscan_file` | 5934 | 4880 |
| policy writes | — | `changed=77` / `phits=102` |

**Proven:** the hook fires and rewrites `scan_control.priority` in 77 of 102
`shrink_node` calls.

**Not proven:** that the policy changes total scan volume — `pgscan_file` is a
running total and the workload is OOM-terminated at a different point each run
(an independent pair gave 4178 vs 6834, the opposite direction). A
fixed-endpoint measurement is the next step; see `docs/MGLRU.md`.

## Known defect that shaped this revision

Registering a kprobe **and** a kretprobe on `get_scan_count` silently suppressed
the return handler (`modified=0`, `nmissed=0`). The kretprobe was removed; the
policy now acts at `shrink_node` entry.

## Safety

Kprobe hooks and policy writes affect reclaim behaviour. QEMU first; `write 0`
before unload; do not `rmmod` on a device until the unload path is proven.

## Build

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

MODE=0 KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/mglru/mglru_probe.ko bash tests/qemu_mglru_gate.sh
```
