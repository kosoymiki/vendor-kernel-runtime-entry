# mglru_probe — Stage A/B reclaim probe (experimental)

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

This is a **probe**, not full MGLRU. It registers kprobes/kretprobes on the 5.10
reclaim path and exposes counters/policy modes through `/proc/mglru_probe`.

- Stage A: read-only call counts for `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`.
- Stage B: `get_scan_count` kretprobe can adjust `nr[]` by policy mode:
  `1` anon-light, `2` file-light, `3` aggressive, `4` generation-aware
  (side table keyed by page).
- `mark_page_accessed` kprobe feeds a small generation table.

**Observed in QEMU (reference):**
- Stage A: 426 hits on reclaim hooks; `page_referenced` 0 in that workload.
- Stage B mode 2: `changed=305`; `before=0,0,1,1` -> `after=0,0,0,0`.
- Stage B mode 4: `acc=10786`, `trk=250`, `yng=250`, `ratio=100`,
  `before=0,0,2,3` -> `after=0,0,1,1`, `changed=368`.

These are experimental probe results, not MGLRU parity. 5.10 has no `gen_lru`
fields; the generation table is a side table. Full 7.4 MGLRU is a staged
backport (`docs/BACKPORT-7.4.md`), not claimed here.

## Safety

Kprobe/kretprobe hooks and scan-count modification can affect reclaim behavior.
QEMU first; disable policy (`write 0`) before any unload; do not `rmmod` on a
device until the unload path is proven.

## Build

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
```
