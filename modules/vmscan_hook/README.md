# vmscan_hook — custom `shrink_lruvec` through a module (experimental)

> Part of a library under **active development** — new modules and stages land
> as they pass their QEMU gates.

Replaces `shrink_lruvec` with a module function and runs the kernel's own
`shrink_active_list` / `shrink_inactive_list` for the pages it chooses. This is
the entry point for a custom reclaim path without rebuilding vmscan.

## Mechanism

- resolve `shrink_lruvec`, `get_scan_count`, `shrink_active_list`,
  `shrink_inactive_list`, `cond_resched` via `kallsyms_lookup_name` (temporary
  kprobe for the resolver itself);
- patch the `shrink_lruvec` entry with `b my_shrink_lruvec` using
  `aarch64_insn_patch_text` (stop_machine);
- `my_shrink_lruvec` calls `get_scan_count`, then loops over
  `INACTIVE_ANON..ACTIVE_FILE`, calling the kernel scanners with
  `min(nr[lru], 32)` and stopping at `nr_to_reclaim`;
- `/proc/vmscan_hook`: write `1` install, `0` restore; read `hooked`, `calls`,
  `scans`, `reclaimed`.

Offsets (verified by `scprobe`): `nr_to_reclaim 0x00`, bits `0x28`,
`gfp_mask 0x30`, `nr_scanned 0x38`, `nr_reclaimed 0x40`; `priority` is `0x2b`
(not `0x2c` — that is `reclaim_idx`). See `docs/OFFSETS.md`.

## Observed (QEMU, stock Image, 512 MiB, swapless)

Gate: `tests/qemu_vmscan_hook_gate.sh`, module sha
`3fb1610cc191b1b5b840aac4140a9c021ac4174d6d5f4eb1f45e5f3c1f7371c9`.

```
insmod_rc=0
hook_rc=0
poll=10 pgscan=8196
vmscan_hook target=... hooked=1 calls=34 scans=8209 reclaimed=0
restore_rc=0 ... hooked=0
rmmod_rc=0  proc_gone  module_gone
```

## Limits

The loop is simplified (no proportional anon/file adjustment, no blk plug);
`reclaimed` is the last call's delta. Only `shrink_lruvec` is replaced; a
custom `shrink_node` is a separate patch target. Text patching is privileged:
QEMU first, `write 0` restores, reboot is the ultimate rollback.

## Build

```sh
KB_MODULE_SRC=$PWD/modules/vmscan_hook KB_MODNAME=vmscan_hook \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
KB_IMAGE=$HOME/tmp/kernel_Image.bin KB_KO=$PWD/modules/vmscan_hook/vmscan_hook.ko \
  bash tests/qemu_vmscan_hook_gate.sh
```
