# vmscan_hook — a custom `shrink_lruvec` injected through a module

This is the entry point for a custom reclaim path without rebuilding vmscan:
the module replaces `shrink_lruvec` with its own function, and that function
runs the kernel's own `shrink_active_list` / `shrink_inactive_list` for the
pages it chooses.

## Why this is possible on 5.10.236

- `CONFIG_KALLSYMS_ALL=y`: `shrink_lruvec`, `get_scan_count`,
  `shrink_active_list`, `shrink_inactive_list` are local symbols but resolvable
  via `kallsyms_lookup_name` (resolved through a temporary kprobe).
- `CONFIG_STRICT_KERNEL_RWX=y`: `aarch64_insn_patch_text` performs the
  stop_machine-safe text update (the same mechanism `kpatch`/`ktramp` use).
- The vendor CFI runtime is satisfied by the module CFI shim (`__cfi_check`,
  `__cfi_jt_*`), `MODVERSIONS` by the generated `__versions`, and the vendor
  `this_module` layout by the postprocess step.
- No ftrace/livepatch is required (and neither is configured).

## Mechanism

```
shrink_lruvec entry (patched: b my_shrink_lruvec)
        |
        v
my_shrink_lruvec(lruvec, sc)              [module]
   get_scan_count(lruvec, sc, nr)         [kernel, via kallsyms]
   while nr[]:
       for lru in INACTIVE_ANON..ACTIVE_FILE:
           shrink_active_list(...)        [kernel, via kallsyms]
           shrink_inactive_list(...)      [kernel, via kallsyms]
       stop when sc->nr_reclaimed - r0 >= sc->nr_to_reclaim
```

Offsets used (verified by `scprobe`, see [`OFFSETS.md`](OFFSETS.md)):
`nr_to_reclaim 0x00`, bits `0x28`, `gfp_mask 0x30`, `nr_scanned 0x38`,
`nr_reclaimed 0x40`.

`/proc/vmscan_hook`: write `1` to install, `0` to restore; read back
`hooked`, `calls`, `scans`, `reclaimed`.

## Observed in QEMU (stock `Image`, 512 MiB, swapless)

`tests/qemu_vmscan_hook_gate.sh`, module sha `3fb1610c…`:

```
insmod_rc=0
hook_rc=0
poll=10 pgscan=8196
vmscan_hook target=... hooked=1 calls=34 scans=8209 reclaimed=0
restore_rc=0
vmscan_hook target=... hooked=0 calls=34 scans=8209 reclaimed=0
rmmod_rc=0
proc_gone
module_gone
```

The custom function ran 34 times under real reclaim, scanned 8209 pages with
the kernel's own list scanners, and the reclaim path stayed alive
(`pgscan_file=8196`). Restore and unload are clean; no panic.

## Composition with `ktramp` (stack gate)

`vmscan_hook` and `ktramp` compose: `my_shrink_lruvec` calls `get_scan_count`,
which is patched by `ktramp`, so the policy-adjusted `nr[]` drives the custom
loop. `tests/qemu_vmscan_stack_gate.sh` loads both, patches both, and runs real
reclaim:

```
poll=14 pgscan=8210
ktramp ... policy=1 calls=112 policy_applied=112 sum20=302 delta20=2 seq=112 ttl=2
vmscan_hook ... hooked=1 calls=112 scans=8579
vh_off=0 kt_off=0 vh_rm=0 kt_rm=0 vh_gone kt_gone vh_mod_gone kt_mod_gone
PASS vmscan stack gate
```

## Limits (honest)

- The loop is intentionally simplified: it does not replicate vmscan's
  proportional anon/file adjustment, `blk_start_plug`, or the `scan_adjusted`
  heuristic. `reclaimed` in the readback is the last call's delta.
- It calls `get_scan_count` for the scan targets; a fully custom generation
  policy can replace that call with module-side computation.
- It replaces `shrink_lruvec` only. `shrink_node_memcgs` still iterates memcgs;
  a custom `shrink_node` is a separate patch target.
- Text patching is a privileged, per-kernel operation: the module refuses to
  patch if the branch is out of range, and `write 0` restores the original
  instruction. QEMU first; reboot is the ultimate rollback.

## Build and gate

```sh
KB_MODULE_SRC=$PWD/modules/vmscan_hook KB_MODNAME=vmscan_hook \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/vmscan_hook/vmscan_hook.ko \
  bash tests/qemu_vmscan_hook_gate.sh
```
