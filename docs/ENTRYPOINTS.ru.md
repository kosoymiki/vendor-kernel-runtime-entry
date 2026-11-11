# Матрица точек входа (с evidence)

Результат runtime-аудита на reference vendor-ядре
(`5.10.236-android12-9-o-g27a48ef6c082`). «Открыто» = доказано в QEMU на exact
stock Image; это не обещание для любого ядра.

| механизм | требует | статус | что даёт | evidence |
|---|---|---|---|---|
| LKM + CFI bridge | `MODULES=y`, unsigned | **доказано** | код модуля, init/exit, callbacks | panic probe + payload `/proc/kbootstrap`, payload_result=42 |
| kprobe / kretprobe | `KPROBES=y` | **доказано** | резолв неэкспортируемых, наблюдение/правка `get_scan_count` | `mglru_probe` 6/6, hits 426; kretprobe changed=305 |
| text patch | `aarch64_insn_patch_text_nosync` + W^X | **доказано в QEMU (1 CPU)** | замена живой функции + restore | `kpatch`: getpid 114 → 0 → 118 |
| трамплин | патчер + `module_alloc`/`set_memory_x` | **доказано в QEMU (1 CPU)** | замена тела функции целиком с вызовом оригинала | `ktramp`: `calls=113`, `pgscan_file=6084`, restore `patched=0` |
| raw payload | `module_alloc` + `set_memory_x` + flush | **доказано** | исполнение доверенного AArch64 | `payload_result=42` |
| kallsyms via kprobe | `KPROBES`, kallsyms | **доказано** | `kallsyms_lookup_name`, module_alloc, set_memory_x, sys_call_table | `/proc/kbootstrap` |
| procfs/sysfs | `proc_create`/params | **доказано** | status/control | `/proc/kbootstrap`, `/proc/kpatch`, `/proc/mglru_probe` |
| BPF | `BPF_SYSCALL/EVENTS/JIT` | **только observe** | tracing; verifier | config; не arbitrary code |
| ftrace function tracer | `FUNCTION_TRACER/DYNAMIC_FTRACE` | **закрыто** | — | config отсутствует |
| livepatch | `LIVEPATCH` | **закрыто** | — | config отсутствует |
| kexec/devmem/multikernel | их configs | **закрыто** | — | config `n` |

## Что реально решает CFI-мост

Vendor: clang 12.0.5 + LTO + `CFI_CLANG=y`. Модуль: clang 21 без LTO. Он
загружается и работает, потому что:

1. `__cfi_check: ret` + `__cfi_jt_init_module`/`__cfi_jt_cleanup_module` +
   `bti c; b init_module` совпадают с ожиданиями `cfi_init()`
   (`kernel/module.c:4530-4545`);
2. `__versions` содержит реальные CRC из running `Image`;
3. relocations `struct module` init/exit пропатчены на vendor offsets;
4. `SHN_XINDEX` исправлен до загрузки.

CFI больше не блокирует «not in 5.10» на уровне загрузки: код модуля,
kprobe-callbacks и пропатченные функции вызываются. **Но** bridge отключает
CFI-проверки **для этого модуля**; это не глобальное отключение. Модуль —
доверенный код.

## Text patch: что доказано, а что нет

`kpatch` (QEMU, exact Image) сохранил первые две инструкции
`__arm64_sys_getpid`, записал `mov w0,#0; ret` через
`aarch64_insn_patch_text_nosync`, проверил `getpid()==0`, затем восстановил
оригинал и получил `118`.

Это доказывает замену функции + rollback. Это **не** доказывает SMP-safe
патчинг:

- `..._nosync` не останавливает другие CPU;
- probe был на single-CPU QEMU;
- на живом SMP нужен `aarch64_insn_patch_text`/stop_machine и проверка под
  нагрузкой;
- неверный patch = panic/corruption; держи restore path; не rmmod до restore.

## Трамплин: что доказано, а что нет

`ktramp` (QEMU, exact Image) копирует первые **7 инструкций** `get_scan_count`
в исполняемую страницу из `module_alloc`, добавляет ветвление обратно на
`target + 28`, патчит вход функции на `b wrap`, а обёртка вызывает трамплин —
так оригинальный код продолжает работать, пока модуль наблюдает и может править
`nr[]`.

Измерено под **реальным reclaim** (ext4-файл через `loop0` + 345 MiB анонимного
давления, см. `docs/MEASURING-RECLAIM.md`), гейт Stage G: холодный кэш
`seq=123`, `ttl=2`, ветка 7 (121 решение полного скана); форс young `y80` —
ветка 6, 51.0%; форс distance `t5` — ветка 1, 100%; форс старых/молодых
вытеснений — ветки 8/9 (51.1%); реальный refault — ветка 2, `tdist=848`;
затем `patched=0`, `rmmod_rc=0`, `proc_gone`, `module_gone`. `vmscan_hook`
добавляет кастомный `shrink_lruvec` (`calls=34`, `scans=8209`, `pgscan=8196`).

Границы:

- QEMU-гейт single-CPU; модуль предпочитает `aarch64_insn_patch_text`
  (stop_machine), когда символ резолвится, и откатывается на `_nosync`;
- ветвление должно быть в пределах ±128 MiB (проверяется, иначе `-ERANGE`);
- скопированный пролог не должен содержать PC-relative инструкций, ломающихся
  при переносе — проверяется на конкретной цели, не в общем виде;
- политика — runtime-модель решений на `get_scan_count`; кастомный reclaim
  path собирается из `kpatch` (тело функции), `ktramp` (трамплин) и kprobe-хуков,
  а не из пересборки vmscan.

## Слой совместимости: без пересборки vmscan

Три базовых модуля — это слой совместимости, а не только probe:

| модуль | какую проблему совместимости решает |
|---|---|
| `kbootstrap` | vendor CFI runtime, резолв kallsyms, vermagic/MODVERSIONS, reloc init/exit `this_module` |
| `kpatch` | замена тела любой достижимой функции, проверка, restore; SMP через `aarch64_insn_patch_text` |
| `ktramp` | трамплин целой функции с вызовом оригинала и policy-хуком на `nr[]` |
| `vmscan_hook` | заменяет `shrink_lruvec` функцией модуля, вызывающей штатные сканеры списков (кастомный reclaim path) |

Точки входа vmscan достижимы без пересборки: kprobes наблюдают `shrink_node`,
`shrink_node_memcgs`, `get_scan_count`, `page_referenced`, `shrink_slab`,
`do_try_to_free_pages`, `workingset_eviction/refault`, `mark_page_accessed`
(mglru_probe регистрирует 6/6 и печатает `reg=1 err=0`); `ktramp` заменяет
`get_scan_count` и вызывает скопированный пролог. Кастомная reclaim-политика —
композиция этих хуков и side table; именно это делает Stage E.

## MGLRU: probe, не parity

`mglru_probe` — Stage A/B: kprobe на reclaim path + priority-политика на
`shrink_node` (mode 5). QEMU: mode 0 `get_scan_count=54`, `pgscan_file=1428`;
mode 5 `get_scan_count=84`, `pgscan_file=4478`, `changed=55/phits=84`
(`gt0=55`, `zero=84`). Stage E — в `ktramp` 1.1.0: per-memcg поколения,
shadow-refault distance, young/cold-тиринг. Это всё ещё не полный 7.4 MGLRU:
в 5.10 нет `gen_lru`; явный gap-list — в
[`BACKPORT-7.4.ru.md`](BACKPORT-7.4.ru.md) и
[`MGLRU-STAGE-E.ru.md`](MGLRU-STAGE-E.ru.md).

## Как это ложится на бэкпорт

- заменить тело функции: text patch/trampoline (доказано в QEMU);
- наблюдать/править аргументы: kprobe/kretprobe (доказано);
- исполнять новый код: raw payload (доказано);
- резолвить символы: kprobe (доказано);
- управление: procfs/params (доказано).

Оставшаяся работа для 7.4 — **сама реализация** и ABI-маппинг, а не механизм
загрузки.
