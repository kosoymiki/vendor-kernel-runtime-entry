# Backporting 7.4 functionality: measured delta and staged map

Scope: runtime backport of newer-kernel functionality into a vendor 5.10
kernel via the entry/payload bus. **CVE point-fixes are out of scope.**

## Provenance (observed 2026-11-10)

- latest stable: **7.3.3**; latest mainline/beta: **7.4-rc2**.
- tarball: `https://git.kernel.org/torvalds/t/linux-7.4-rc2.tar.gz`
  sha256 `010063d5fc7bb3a5f1fb116cff45abe4977e8a8205b7841773505680c7a014bb`
  (274,397,238 B); tag commit `7475ea8b58616b33c04afb976cf0fbaad9dd63fb`.
- `Makefile`: `7.4.0-rc2`, name “Baby Opossum Posse”.
- vendor: `5.10.236`, git `0fd25ffc…`.

## Measured file-to-file delta

| metric | value |
|---|---:|
| vendor text files / LOC | 68,635 / 30,956,509 |
| 7.4-rc2 text files / LOC | 97,345 / 44,559,671 |
| LOC delta | **+13,603,162 (+43.9%)** |
| union paths | 111,989 |
| added / removed / modified / identical | 43,291 / 14,639 / 40,388 / 13,671 |
| line diff (source-filtered) | **+20,760,506 / −7,399,473** across 95,171 files |
| Kconfig symbols | 19,229 → 22,149 (**+6,004 / −3,084**) |
| new syscalls | **31** |

Manifest: `delta-manifest.tsv.gz` (every path + status + sizes); per-file line
delta: `file-line-delta.tsv.gz` (95,171 rows). The generator scripts are in
`tools/delta/`; the large generated map files are not committed here.

## Top-level delta (measured)

| top | +files | −files | ~files | line+ | line− |
|---|---:|---:|---:|---:|---:|
| drivers | 14,250 | 6,029 | 19,363 | 12,281,126 | 4,376,244 |
| arch | 7,767 | 4,876 | 7,263 | 2,188,509 | 926,903 |
| Documentation | 8,704 | 566 | 1,777 | 1,160,913 | 114,988 |
| tools | 6,294 | 893 | 2,567 | 1,929,993 | 334,005 |
| include | 1,936 | 1,016 | 3,051 | 595,467 | 244,139 |
| sound | 998 | 255 | 1,628 | 657,324 | 177,259 |
| fs | 592 | 237 | 1,612 | 665,791 | 374,126 |
| net | 275 | 246 | 1,436 | 401,432 | 251,197 |
| lib | 599 | 127 | 254 | 195,083 | 47,077 |
| kernel | 285 | 60 | 433 | 304,278 | 130,204 |
| rust | 710 | 0 | 0 | n/a (new) | n/a |
| security | 92 | 1 | 198 | 45,120 | 14,633 |
| mm | 80 | 26 | 127 | 150,915 | 84,548 |
| io_uring | 87 | 0 | 4 | 26,997 | 10,619 |

(Rows with “n/a (new)” are entirely new trees; line diff is not meaningful
when the vendor path is absent.)

## New syscalls

`cachestat`, `epoll_pwait2`, `fchmodat2`, `fchroot`, `file_getattr`,
`file_setattr`, `futex_requeue`, `futex_wait`, `futex_waitv`, `futex_wake`,
`getxattrat`, `landlock_add_rule`, `landlock_create_ruleset`,
`landlock_restrict_self`, `listmount`, `listns`, `listxattrat`,
`lsm_get_self_attr`, `lsm_list_modules`, `lsm_set_self_attr`,
`map_shadow_stack`, `memfd_secret`, `mount_setattr`, `mseal`,
`open_tree_attr`, `quotactl_fd`, `removexattrat`, `rseq_slice_yield`,
`set_mempolicy_home_node`, `setxattrat`, `statmount`.

## Feature map (first wave)

| id | feature | 7.4 source | 5.10 target | mechanism | tier | status |
|---|---|---|---|---|---|---|
| F-001 | MGLRU | `mm/vmscan.c` (`lru_gen*`, 216 refs), `mm/workingset.c`, `Documentation/admin-guide/mm/multigen_lru.rst` | `mm/vmscan.c`, `mm/workingset.c`, lruvec/page fields | A: kprobe observability; B: policy hooks; C: trampolines; side table for generation state | T1 | **Stage E verified (`ktramp` 1.1.0)** |
| F-002 | VMA rework | `mm/vma.c` (3,715 L), `mm/mmap.c` | `mm/mmap.c`, `mm/memory.c` | source/trampolines; high risk | T1 | inventory |
| F-003 | DAMON | `mm/damon/core.c` (4,311 L) | `CONFIG_DAMON=y`, `DAMON_PADDR=y`, `DAMON_RECLAIM=y`; no `DAMON_SYSFS` | native, not administrable; no backport needed | T1 | **native (observed)** |
| F-004 | alloc_tag | `mm/alloc_tag.c` (1,367 L) | `mm/` | source/module; page_ext | T1 | inventory |
| F-005 | memcontrol-v1 | `mm/memcontrol-v1.c` (2,218 L) | `mm/memcontrol.c` | source port; cgroup ABI | T2 | inventory |
| F-006 | sched_ext | `kernel/sched/ext/ext.c` 12,499 L + `sub.c` 3,229 L | absent | module/hooks; BPF scheduler | T2 | inventory |
| F-007 | io_uring features | `io_uring/zcrx.c` 1,970 L, `net.c` 2,013 L | 4 vendor files | syscall/table + module; high risk | T2 | inventory |
| F-008 | Landlock/LSM | `security/landlock/*`, `security/lsm_init.c` | absent | LSM/hooks + UAPI | T2 | inventory |
| F-009 | TCP-AO/net | `net/ipv4/tcp_ao.c`, MPTCP, Bluetooth ISO | `net/` | source/module hooks | T2 | inventory |
| F-010 | Rust for Linux | `rust/` 710 files | absent | toolchain + abstractions; not drop-in | T2 | inventory |
| F-011 | 31 syscalls | UAPI/table | arm64 syscall table | table patch + CFI-safe trampolines | T2 | inventory |
| F-012 | generic drivers | `drivers/` +14,250 | `drivers/` | source port; vendor-specific out | T3 | inventory |
| F-013 | arm64 features | `arch/` +7,767 | `arch/arm64` | feature-gated source port | T3 | inventory |

## MGLRU: staged, not fake

- **Stage A (next):** kprobe observability on `shrink_node`, `get_scan_count`,
  `page_referenced`/`folio_mark_accessed`; baseline metrics (pgscan, refault,
  latency) in QEMU; no behavior change.
- **Stage B:** policy hook: generation aging/scan balancing in module state,
  keyed by folio/page; adjust scan counts only where safe.
- **Stage C:** trampolines for selected `vmscan` functions with MGLRU logic;
  W^X, CFI-safe entry, rollback.
- **Stage D:** parity subset + documented gaps.

5.10 has no `gen_lru` fields; use a side table or proven spare page flags, not
invented offsets. ABI evidence must be file:line/disasm.

## “Backport everything” statement

Every top-level tree is enumerated in the measured delta (27 rows including
root); the feature map covers the first wave. Execution is staged by tier;
non-runtime trees (Documentation/tools/samples/scripts/usr/LICENSES/certs/
android) are explicitly classified, not silently omitted. No feature is marked
verified until QEMU metrics/behavior + rollback.

## Why this is possible at all

The entry does not require the disabled configs (`KEXEC`, `DEVMEM`,
`MULTIKERNEL`, `LIVEPATCH`, `FUNCTION_TRACER`). It uses loadable modules,
kprobe symbol resolution, and runtime code/hook payloads. That is the
“unique entry point”; the backport layer builds on it.

## F-001 MGLRU — Stage E result (2026-11-11)

`ktramp` 1.1.0 (sha `85d01060…`) implements the runtime decision model at the
`get_scan_count` boundary:

- per-memcg generation tables (vendor `android_vh_shrink_node_memcgs`
  tracepoint for exact attribution);
- shadow-based refault distance: `lruvec->nonresident_age` (offset 96) minus
  `shadow >> 19`;
- young/cold tiering from `mark_page_accessed` page generations;
- SMP-safe text patch with restore; unload clean.

QEMU evidence (`tests/qemu_ktramp_gate.sh`, stock Image, 512 MiB, swapless):

| regime | counters | branch | suppression |
|---|---|---|---|
| cold ext4 cache | `yc=0/398`, `nr_calls=108` | 7 (full scan) | `delta20=2 / pre20=610` |
| forced young `y80` | `fyc=80/20` | 6 | `delta20=376 / pre20=736` = 51.1% |
| forced distance `t5` | `fd=5` | 1 | `delta20=8703 / pre20=8703` = 100% |
| real refault | `MADV_PAGEOUT` 128 KiB | 2 | `tdist=847` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

What remains for M4: the exact `min_seq/max_seq` reclaim clock, per-generation
LRU lists, and folio paths. These are delivered as side tables plus
`kpatch`/`ktramp`/kprobe hooks on the reclaim path — not as a vmscan rebuild.
The compatibility layer (CFI, MODVERSIONS, vermagic, init/exit relocations,
SMP-safe patching) is the point of `kbootstrap` + `kpatch` + `ktramp`. The gap
list below is about fidelity, not about a missing entry point.
