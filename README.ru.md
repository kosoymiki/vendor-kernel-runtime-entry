# Runtime Entry для vendor-ядра Linux

**[English](README.md) | Русский**

Запуск доверенного кода в **замороженном вендорском ядре** в рантайме — без
пересборки ядра, без `KEXEC`, `DEVMEM`, `MULTIKERNEL`, `LIVEPATCH` и
`FUNCTION_TRACER`, без разблокировки загрузчика.

Это **reference implementation + per-target builder**, а не универсальный
модуль. Доказано в QEMU на Realme GT3 (RMX3709), vendor-ядро
`5.10.236-android12-9-o-g27a48ef6c082`.

> **Безопасность:** это исполнение произвольного кода в ядре. Сначала прочитай
> [`docs/SECURITY.md`](docs/SECURITY.md). Плохой модуль может вызвать panic или
> кирпич. Сначала QEMU; на устройстве не делай `rmmod`, пока unload-path не
> доказан.

## В чём была стена

Функции ядра — это compile-time переключатели (`=y` / `=m` / `=n`). Если вендор
поставил `CONFIG_KEXEC=n`, `CONFIG_DEVMEM=n`, убрал livepatch/ftrace — включить
их в рантайме нельзя, а заблокированный загрузчик не даёт прошить новое ядро.

Мы не включаем эти символы. Мы используем **дверь модулей** (`CONFIG_MODULES=y`)
и решаем всё, что мешало ей работать на vendor-ядре:

1. точный **vermagic** и `.modinfo`;
2. **`__versions`/CRC** из running `Image` + `/proc/kallsyms`;
3. смещения **`struct module`** (init/exit), патч по имени символа;
4. **CFI**-мост (`__cfi_check`, `__cfi_jt_*`, jump tables);
5. баг **`SHN_XINDEX`** (`ld.lld` без `SYMTAB_SHNDX`);
6. исполняемая память (`module_alloc` + `set_memory_x` + flush);
7. резолв символов через **kprobe** (`kallsyms_lookup_name` не экспортируется);
8. пакет **KernelSU/ReSukiSU**.

## Возможности (всё доказано в QEMU на reference Image)

- **LKM + CFI bridge**: init/exit и callbacks модуля исполняются;
- **kprobe/kretprobe**: резолв неэкспортируемых; наблюдение/правка аргументов;
- **text patch**: замена живой функции + restore (`kpatch` probe);
- **raw payload**: `module_alloc` + `set_memory_x`, `payload_result=42`;
- **procfs/sysfs**: status/control.

Полная матрица evidence: [`docs/ENTRYPOINTS.ru.md`](docs/ENTRYPOINTS.ru.md)
([EN](docs/ENTRYPOINTS.md)). Статус MGLRU: [`docs/MGLRU.ru.md`](docs/MGLRU.ru.md)
([EN](docs/MGLRU.md)).

## Доказательство (QEMU, exact stock Image)

Panic probe — `init_module` реально исполняется:

```
cfi: not using shadow for module kbootstrap
Kernel panic - not syncing: kbootstrap entered
  init_module+0x1c/0xfe8 [kbootstrap]
  do_one_initcall+0x114/0x458
  do_init_module+0x64/0x8cc
  load_module+0x3650/0x3f44
```

Payload bus — raw AArch64 код исполняется, есть side effect:

```
insmod_rc=0
kbootstrap: ready kln=... alloc=... flush=... sct=... payload=42
/proc/kbootstrap:
  kallsyms_lookup_name=...
  module_alloc=...
  set_memory_x=...
  sys_call_table=...
  payload_result=42
  proc_entry=created
```

Эталонный артефакт: `kbootstrap.ko` sha256
`065f520b1b562f55eef48a138a12d4df3c18d51357bcbacb39b46d1e9a33c593`.
QEMU gate: `tests/qemu_payload_gate.sh` → `PASS qemu payload gate`.

## Быстрый старт — только reference target

```sh
ksud module install module.zip   # release asset, только RMX3709 5.10.236
# reboot
cat /proc/kbootstrap
```

**Не ставь этот `.ko` на другое ядро.** Он привязан к vermagic, CRC, struct
offsets и CFI mode reference-ядра.

## Адаптация под другое ядро

Builder сам определяет config/ABI/vermagic и **отказывается угадывать**:

```sh
git clone https://github.com/kosoymiki/vendor-kernel-runtime-entry.git
cd vendor-kernel-runtime-entry
tools/collect_target.sh target/
# заполнить profiles/my-device/profile.env (Image, kallsyms, vendor .ko, offsets)
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Подробно, по шагам, с troubleshooting:
[`docs/ADAPTING.ru.md`](docs/ADAPTING.ru.md). English:
[`docs/ADAPTING.md`](docs/ADAPTING.md).

## Что работает, а что нет

**Работает, если:** `CONFIG_MODULES=y`; неподписанные модули разрешены
(`MODULE_SIG_FORCE` не `y`); `CONFIG_KPROBES=y`; есть target `Image` и
`kallsyms` (или точный `Module.symvers`); ABI offsets можно вывести.

**Не работает, если:** `CONFIG_MODULES=n` (нет LKM-входа);
`CONFIG_MODULE_SIG_FORCE=y` (обхода нет); `CONFIG_KPROBES=n`; нет ABI-evidence.
CFI-мост отключает CFI-проверки **для этого модуля** — осознанный module-local
компромисс, не глобальный обход.

## Карта репозитория

```
kernel/      базовый модуль + CFI bridge
modules/     kpatch (text patch probe), mglru (Stage A/B probe)
module/      KernelSU/ReSukiSU пакет
tools/       per-target builder, ELF/ABI/CRC gates, delta tooling
profiles/    профили (reference: RMX3709 5.10.236)
tests/       static checks + QEMU payload gate
docs/        adaptation, entry matrix, MGLRU, methodology, security, backport map
```

## Патч CVE в рантайме

Defensive-шаблон: как превратить upstream-фикс в runtime-митигацию
(kprobe argument guard; trampoline — только для узких случаев) и загрузить
через тот же entry. Эксплойт-кода нет.

- [English](docs/CVE-PATCHING.md)
- [Русский](docs/CVE-PATCHING.ru.md)

## Бэкпорт 7.4

Измеренная дельта 5.10.236 → 7.4-rc2: **+13.6M LOC** (vendor 30.96M vs latest
44.56M), union **111,989** путей (43,291 added / 14,639 removed / 40,388
modified), line diff **+20.76M / −7.40M**, Kconfig **+6,004/−3,084**, 31 новый
syscall. MGLRU — первый feature/benchmark; CVE — вне roadmap бэкпорта 7.4
(отдельный defensive-шаблон патча — по ссылке выше). См.
[`docs/BACKPORT-7.4.md`](docs/BACKPORT-7.4.md).

## Релиз

- Репозиторий: <https://github.com/kosoymiki/vendor-kernel-runtime-entry>
- v1.0.2: <https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.2>

## Лицензия

GPL-2.0-only. Модули ядра должны быть GPL-совместимы.
