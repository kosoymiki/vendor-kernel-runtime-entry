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
- Mode 5 writes `scan_control.priority` (offset `0x2b`) to `0` at `shrink_node`
  entry and counts entries (`phits`) and real changes (`changed`).

## Observed in QEMU (exact stock Image)

Gate: `tests/qemu_mglru_gate.sh` (ext4 file through `loop0` + 345 MiB anonymous
pressure, poll `pgscan_file`; see `docs/MEASURING-RECLAIM.md`).

| metric | mode 0 | mode 5 |
|---|---|---|
| `get_scan_count` hits | 109 | 11 |
| `pgscan_file` | 6071 | 2464 |
| policy writes | — | `changed=11` / `phits=11` (`gt0=11`, `zero=11`) |

**Proven:** the hook fires and rewrites the real `scan_control.priority`
(offset `0x2b`, verified by `scprobe`: 12→11→10→9) in 11 of 11 `shrink_node`
calls.

**Not a metric:** cumulative `pgscan_file` is a running total and the workload
is OOM-terminated at a different point each run (an independent pair gave 4178
vs 6834, the opposite direction). The fixed-endpoint `delta/pre` metric lives in
`ktramp` 1.1.0 (Stage E); see `docs/MGLRU-STAGE-E.md`.

## Known defects that shaped this revision

- Registering a kprobe **and** a kretprobe on `get_scan_count` silently
  suppressed the return handler (`modified=0`, `nmissed=0`). The kretprobe was
  removed; the policy now acts at `shrink_node` entry.
- The priority offset was `0x2c` in an earlier revision; `scprobe` showed
  `0x2c` is `reclaim_idx` and `priority` is `0x2b`. Fixed; see
  `docs/OFFSETS.md`.
- `mark_page_accessed` (`mkp`) was registered but not unregistered in `exit`,
  so `rmmod` left a breakpoint into freed module text and the next access
  Oopsed (`kprobe_breakpoint_handler`, "last unloaded: mglru_probe"). Fixed with
  an `mk_reg` flag; the load/read/rmmod gate is green.

Artifact: `mglru_probe.ko`, sha256
`754d20b990bcd022e8766cbe79676eec847dcdfe50e05f99af660dcddf940ea4`.

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
