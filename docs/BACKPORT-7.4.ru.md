# Backport функциональности 7.4: измеренная дельта и поэтапная карта

Область: runtime-backport функциональности нового ядра в vendor-ядро 5.10 через
entry/payload bus. **CVE point-fixes вне области.**

## Provenance (наблюдено 2026-11-10)

- последний стабильный: **7.3.3**; последний mainline/beta: **7.4-rc2**.
- tarball: `https://git.kernel.org/torvalds/t/linux-7.4-rc2.tar.gz`
  sha256 `010063d5fc7bb3a5f1fb116cff45abe4977e8a8205b7841773505680c7a014bb`
  (274,397,238 B); commit тега `7475ea8b58616b33c04afb976cf0fbaad9dd63fb`.
- `Makefile`: `7.4.0-rc2`, имя “Baby Opossum Posse”.
- vendor: `5.10.236`, git `0fd25ffc…`.

## Измеренная дельта файл-в-файл

| метрика | значение |
|---|---:|
| vendor текстовых файлов / LOC | 68,635 / 30,956,509 |
| 7.4-rc2 текстовых файлов / LOC | 97,345 / 44,559,671 |
| дельта LOC | **+13,603,162 (+43.9%)** |
| объединённых путей | 111,989 |
| добавлено / удалено / изменено / идентично | 43,291 / 14,639 / 40,388 / 13,671 |
| построчный diff (source-filtered) | **+20,760,506 / −7,399,473** по 95,171 файлам |
| символов Kconfig | 19,229 → 22,149 (**+6,004 / −3,084**) |
| новых syscall | **31** |

Манифест: `delta-manifest.tsv.gz` (каждый путь + статус + размеры); построчная
дельта: `file-line-delta.tsv.gz` (95,171 строк). Скрипты-генераторы — в
`tools/delta/`; большие сгенерированные карты здесь не коммитятся.

## Дельта верхнего уровня (измерено)

| top | +файлов | −файлов | ~файлов | строк+ | строк− |
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
| rust | 710 | 0 | 0 | n/a (новое) | n/a |
| security | 92 | 1 | 198 | 45,120 | 14,633 |
| mm | 80 | 26 | 127 | 150,915 | 84,548 |
| io_uring | 87 | 0 | 4 | 26,997 | 10,619 |

(Строки «n/a (новое)» — полностью новые деревья; построчный diff не имеет
смысла, когда пути в vendor отсутствуют.)

## Новые syscall

`cachestat`, `epoll_pwait2`, `fchmodat2`, `fchroot`, `file_getattr`,
`file_setattr`, `futex_requeue`, `futex_wait`, `futex_waitv`, `futex_wake`,
`getxattrat`, `landlock_add_rule`, `landlock_create_ruleset`,
`landlock_restrict_self`, `listmount`, `listns`, `listxattrat`,
`lsm_get_self_attr`, `lsm_list_modules`, `lsm_set_self_attr`,
`map_shadow_stack`, `memfd_secret`, `mount_setattr`, `mseal`,
`open_tree_attr`, `quotactl_fd`, `removexattrat`, `rseq_slice_yield`,
`set_mempolicy_home_node`, `setxattrat`, `statmount`.

## Карта фич (первая волна)

| id | фича | источник 7.4 | цель 5.10 | механизм | tier | статус |
|---|---|---|---|---|---|---|
| F-001 | MGLRU | `mm/vmscan.c` (`lru_gen*`, 216 ссылок), `mm/workingset.c`, `Documentation/admin-guide/mm/multigen_lru.rst` | `mm/vmscan.c`, `mm/workingset.c`, поля lruvec/page | A: kprobe-наблюдаемость; B: policy-хуки; C: трамплины; side table для поколений | T1 | **Stage E verified (`ktramp` 1.1.0)** |
| F-002 | VMA rework | `mm/vma.c` (3,715 L), `mm/mmap.c` | `mm/mmap.c`, `mm/memory.c` | source/трамплины; высокий риск | T1 | inventory |
| F-003 | DAMON | `mm/damon/core.c` (4,311 L) | `CONFIG_DAMON=y`, `DAMON_PADDR=y`, `DAMON_RECLAIM=y`; нет `DAMON_SYSFS` | native, не административен; backport не нужен | T1 | **native (наблюдено)** |
| F-004 | alloc_tag | `mm/alloc_tag.c` (1,367 L) | `mm/` | source/module; page_ext | T1 | inventory |
| F-005 | memcontrol-v1 | `mm/memcontrol-v1.c` (2,218 L) | `mm/memcontrol.c` | source port; cgroup ABI | T2 | inventory |
| F-006 | sched_ext | `kernel/sched/ext/ext.c` 12,499 L + `sub.c` 3,229 L | отсутствует | module/хуки; BPF-планировщик | T2 | inventory |
| F-007 | io_uring features | `io_uring/zcrx.c` 1,970 L, `net.c` 2,013 L | 4 vendor-файла | syscall/table + модуль; высокий риск | T2 | inventory |
| F-008 | Landlock/LSM | `security/landlock/*`, `security/lsm_init.c` | отсутствует | LSM/хуки + UAPI | T2 | inventory |
| F-009 | TCP-AO/net | `net/ipv4/tcp_ao.c`, MPTCP, Bluetooth ISO | `net/` | source/module-хуки | T2 | inventory |
| F-010 | Rust for Linux | `rust/` 710 файлов | отсутствует | toolchain + абстракции; не drop-in | T2 | inventory |
| F-011 | 31 syscall | UAPI/table | таблица syscall arm64 | патч таблицы + CFI-safe трамплины | T2 | inventory |
| F-012 | generic drivers | `drivers/` +14,250 | `drivers/` | source port; vendor-specific вне | T3 | inventory |

## F-001 MGLRU — результат Stage E (2026-11-11)

`ktramp` 1.1.0 (sha `85d01060…`) реализует runtime-модель решений на границе
`get_scan_count`:

- per-memcg таблицы поколений (vendor-tracepoint
  `android_vh_shrink_node_memcgs` для точной атрибуции);
- shadow-refault distance: `lruvec->nonresident_age` (offset 96) минус
  `shadow >> 19`;
- young/cold-тиринг по поколениям страниц из `mark_page_accessed`;
- SMP-safe патч текста с restore; чистый unload.

QEMU-доказательства (`tests/qemu_ktramp_gate.sh`, stock Image, 512 MiB, без
swap):

| режим | счётчики | ветка | подавление |
|---|---|---|---|
| холодный ext4-кэш | `yc=0/398`, `nr_calls=108` | 7 (полный скан) | `delta20=2 / pre20=610` |
| форс young `y80` | `fyc=80/20` | 6 | `delta20=376 / pre20=736` = 51.1% |
| форс distance `t5` | `fd=5` | 1 | `delta20=8703 / pre20=8703` = 100% |
| реальный refault | `MADV_PAGEOUT` 128 KiB | 2 | `tdist=847` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

Что остаётся для M4: точные `min_seq/max_seq`-часы reclaim, per-generation
LRU-списки и folio-пути. Они делаются через side table плюс хуки
`kpatch`/`ktramp`/kprobe на reclaim path — без пересборки vmscan. Слой
совместимости (CFI, MODVERSIONS, vermagic, reloc init/exit, SMP-safe патчинг) —
и есть смысл `kbootstrap` + `kpatch` + `ktramp`. Gap-list ниже — про точность
модели, а не про отсутствие точки входа.
