# Матрица точек входа (с evidence)

Результат runtime-аудита на reference vendor-ядре
(`5.10.236-android12-9-o-g27a48ef6c082`). «Открыто» = доказано в QEMU на exact
stock Image; это не обещание для любого ядра.

| механизм | требует | статус | что даёт | evidence |
|---|---|---|---|---|
| LKM + CFI bridge | `MODULES=y`, unsigned | **доказано** | код модуля, init/exit, callbacks | panic probe + payload `/proc/kbootstrap`, payload_result=42 |
| kprobe / kretprobe | `KPROBES=y` | **доказано** | резолв неэкспортируемых, наблюдение/правка `get_scan_count` | `mglru_probe` 6/6, hits 426; kretprobe changed=305 |
| text patch | `aarch64_insn_patch_text_nosync` + W^X | **доказано в QEMU (1 CPU)** | замена живой функции + restore | `kpatch`: getpid 114 → 0 → 118 |
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

## MGLRU: probe, не parity

`mglru_probe` — Stage A/B: kprobe на reclaim path + kretprobe на
`get_scan_count` с режимами; side table поколений для mode 4. QEMU: Stage A
426 hits; mode 2 `changed=305`; mode 4 `acc=10786`, `trk=250`, `yng=250`,
`ratio=100`, `before=0,0,2,3` → `after=0,0,1,1`. Это эксперимент, не полный
7.4 MGLRU. В 5.10 нет `gen_lru`; см. `docs/MGLRU.md`.

## Как это ложится на бэкпорт

- заменить тело функции: text patch/trampoline (доказано в QEMU);
- наблюдать/править аргументы: kprobe/kretprobe (доказано);
- исполнять новый код: raw payload (доказано);
- резолвить символы: kprobe (доказано);
- управление: procfs/params (доказано).

Оставшаяся работа для 7.4 — **сама реализация** и ABI-маппинг, а не механизм
загрузки.
