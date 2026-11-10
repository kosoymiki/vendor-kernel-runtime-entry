# Адаптация под другое vendor-ядро — подробная инструкция

Это длинная версия. Короткая — в `README.ru.md`. Builder **не** делает
универсальный бинарник: он берёт факты у target-ядра и собирает/нормализует
модуль под него. Если факта нет — он останавливается, а не угадывает.

---

## 0. Что нужно

| что | зачем |
|---|---|
| исходники ядра, максимально совпадающие с running kernel | kbuild, headers, config |
| build-дерево с `.config` | флаги/заголовки для сборки |
| running config `/proc/config.gz` | проверка CFI/MODVERSIONS/KPROBES/MODULES |
| несжатый `Image` | восстановление CRC/проверка ABI |
| дамп `/proc/kallsyms` | адреса/имена/CRC-таблицы |
| known-good vendor `.ko` | ABI-oracle: vermagic, init/exit offsets, CFI mode |
| `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`, `make`, `python3`, `zip` | toolchain |
| `qemu-system-aarch64` + static busybox | безопасный gate до устройства |

Если vendor `.ko` нет — offsets можно вывести через QEMU+gdb (см. §4), но
builder всё равно требует явные `--init-off/--exit-off`.

---

## 1. Собрать факты target

```sh
tools/collect_target.sh target/
# target/config, target/kallsyms, target/version, target/uname
```

На Android нужен root для `/proc/config.gz` и `/proc/kallsyms`.

**Image:** достань несжатый kernel из `boot.img`/`vendor_boot.img` твоими
инструментами (`magiskboot`, `unpack_bootimg`, vendor dumper). Оставь именно
тот `Image`, который работает. Извлечение device-specific и намеренно не
автоматизировано. Не бери Image от другой сборки.

**Vendor `.ko` oracle:** возьми модуль, который точно загружается на устройстве
(`/vendor/lib/modules`, vendor/KernelSU module dir). Проверь:

```sh
readelf -p .modinfo vendor.ko | grep -E 'vermagic|name='
readelf -SW vendor.ko | grep .gnu.linkonce.this_module
readelf -rW vendor.ko | sed -n '/this_module/,/^$/p'
readelf -sW vendor.ko | grep -E '__cfi|cfi_jt'
```

Relocation offsets `init_module`/`cleanup_module` — это ABI ground truth для
`struct module`. На reference-ядре: `0x190`/`0x3c0`, size `0x400`.

---

## 2. Build-дерево

Нужно дерево, достаточно близкое к running kernel для headers/kbuild. Это не
обязательно полный vendor build, но версия/конфиг-семейство должны совпадать.

```sh
export KERNEL_SRC=$HOME/my-kernel
export KERNEL_OUT=$HOME/my-kernel-out
mkdir -p "$KERNEL_OUT"

cp target/config "$KERNEL_OUT/.config"

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error modules_prepare
```

`olddefconfig` может выкинуть vendor-опции, потому что локальное дерево/clang
другие. Поэтому в профиле есть **`KB_CONFIG`** — running config для решений о
фичах (CFI, MODVERSIONS, KPROBES), отдельно от build `.config`, который нужен
только для компиляции.

Если `modules_prepare` падает на warning-as-error — `KCFLAGS=-Wno-error` (или
узкий `-Wno-error=<warning>`); не отключай CFI/LTO только ради сборки.

---

## 3. Vermagic

Бери точную строку из `.modinfo` vendor `.ko` или из `/proc/version` + target
build. Там есть `SMP`, `preempt`, `mod_unload`, `modversions`, arch-suffix.

Неверный vermagic → “no version magic”. Builder нормализует `.modinfo`, но
строку не выдумывает.

---

## 4. ABI `struct module` (init/exit offsets)

Локальное дерево может разложить `struct module` иначе (CFI/LTO/опции).
Running kernel читает `mod->init`/`mod->exit` по фиксированным offsets.

**С vendor `.ko`:**

```sh
readelf -rW vendor.ko | sed -n '/this_module/,/^$/p'
# init_module + 0 -> offset
# cleanup_module + 0 -> offset
```

**Без vendor `.ko`:** QEMU+gdb на точном Image:

```
break do_init_module
# смотри mod и mod->init; дизассемблируй do_init_module, чтобы увидеть offset
```

Передай `--init-off`/`--exit-off` (или в профиль). Builder патчит relocation
**по имени символа** (`fix_this_module_relocs.py`), а не по «магическому» offset.

---

## 5. `__versions` / CRC

Если `CONFIG_MODVERSIONS=y`, пустая/отсутствующая `__versions` может дать
silent `-ENOEXEC`: `CONFIG_MODULE_FORCE_LOAD` не включён. Варианты:

### A. Точный `Module.symvers`

Возьми `Module.symvers` из точной сборки ядра и положи в
`$KERNEL_OUT/Module.symvers`. Лучший вариант.

### B. Восстановление из `Image` + kallsyms

Builder может запустить `tools/symvers_from_image.py` при
`KB_REGENERATE_SYMVERS=1` и заданном `KB_LINK_BASE`. Он декодирует
`__ksymtab`/`__kcrctab` из Image.

`KB_LINK_BASE` — link-time адрес `_head`/`_text` для Image. Он device/build
specific. Возьми из matching `vmlinux` (`nm vmlinux | grep ' _head\| _text'`)
или vendor build metadata. Если установить нельзя — используй точный
`Module.symvers`; не гадай.

Финальный gate `verify_versions.py` проверяет: каждый import присутствует,
экспортирован, CRC точный. Неверная таблица = hard fail.

---

## 6. CFI

Проверь running config:

```sh
grep -E 'CONFIG_(CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH)' target/config
```

Если `CONFIG_CFI_CLANG=y` — собирай с `KB_CFI=1` (профиль может ставить
`KB_FORCE_CFI=1`, если build-tree config не равен running config). Bridge даёт:

- `init_module.cfi_jt`/`cleanup_module.cfi_jt` → `bti c; b <target>`
- `__cfi_jt_init_module`/`__cfi_jt_cleanup_module` → указатели на них
- `__cfi_check` → `ret`
- `__ubsan_handle_cfi_check_fail_abort` → panic stub

Vendor `cfi_init()` находит это и переписывает `mod->init/exit`. Модуль
принимается, но **CFI-проверки для этого модуля отключаются**. Это module-local
компромисс; грузи только доверенный код.

Если CFI нет — `KB_CFI=0`, bridge не линкуется.

---

## 7. Профиль

Скопируй reference и поменяй всё:

```sh
cp profiles/rmx3709-5.10.236/profile.env profiles/my-device/profile.env
```

Типичные значения:

```sh
KB_PROFILE_NAME="my-device"
KB_KERNEL_SRC="$HOME/my-kernel"
KB_KERNEL_OUT="$HOME/my-kernel-out"
KB_CONFIG="$HOME/target/config"       # running config
KB_IMAGE="$HOME/target/Image"         # exact Image
KB_KALLSYMS="$HOME/target/kallsyms"
KB_VENDOR_KO="$HOME/target/vendor.ko" # ABI oracle (или пусто + offsets)
KB_VERMAGIC="<exact>"
KB_INIT_OFF=0x...
KB_EXIT_OFF=0x...
KB_LINK_BASE=0x...                    # только для восстановления symvers
KB_MODULE_CFLAGS=""                   # reference audit выключен для переносимости
```

Builder читает `KB_CONFIG` для фич, `KERNEL_OUT/.config` — для компиляции.

---

## 8. Сборка + нормализация + gate

```sh
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Что делает builder:

1. проверяет `MODULES=y`, `MODULE_SIG_FORCE != y`, `KPROBES=y`;
2. определяет CFI/MODVERSIONS из `KB_CONFIG`;
3. берёт vermagic/offsets из `KB_VENDOR_KO` (или явных аргументов);
4. использует точный `Module.symvers` или восстанавливает из Image/kallsyms;
5. собирает target kbuild-деревом (`KB_CFI` как make-переменная);
6. запускает `fix_symtab_shndx.py` (ld.lld SHN_XINDEX fix);
7. запускает `postprocess_common.sh` (versions gate, modinfo, relocations,
   финальные проверки);
8. пишет `dist/.../{kbootstrap.ko,module.zip,manifest.json}`;
9. опционально запускает QEMU payload gate.

Если остановился — читай точную ошибку; не «исправляй» удалением gate.

---

## 9. QEMU gate (до устройства)

```sh
KB_IMAGE=$HOME/target/Image \
KB_KO=$PWD/kernel/kbootstrap.ko \
BUSYBOX=$HOME/ae-qemu-gate/bin/busybox \
tests/qemu_payload_gate.sh
```

Он поднимает точный Image с минимальным initramfs и проверяет:

- `insmod_rc=0`;
- модуль live;
- `/proc/kbootstrap` существует;
- `payload_result=42`;
- нет CFI failure на init.

Ожидаемый безвредный warning: `cfi: not using shadow for module kbootstrap`.
Gate не доказывает unload/`rmmod` и hardware-specific поведение.

---

## 10. Установка на устройство

```sh
ksud module install dist/<profile>/module.zip
# reboot
cat /proc/kbootstrap
```

`post-fs-data.sh` грузит модуль; `service.sh` пишет статус. **Не делай
`rmmod`**: непроверенный unload/CFI путь может дать panic. Rollback = reboot.

Перед установкой: backup boot/vendor_boot и знай, как восстановить. Лучше
тестовое устройство.

---

## Troubleshooting

| симптом | причина | действие |
|---|---|---|
| `no symbol version` / silent `-ENOEXEC` | пустая/нет `__versions` | точный `Module.symvers` или восстановление из Image; `verify_versions.py` |
| `disagrees about version of symbol` | неверные CRC | target Image/kallsyms; не переиспользуй чужую таблицу |
| `no version magic` | неверный vermagic | возьми из vendor `.ko`; нормализуй `.modinfo` |
| CFI failure при load/rmmod | нет/неверный bridge | `KB_CFI=1`, `kbootstrap_cfi*`, проверь `__cfi_jt_*` |
| Oops `load_module+0x20d4` | `SHN_XINDEX` без `SYMTAB_SHNDX` | `fix_symtab_shndx.py` до postprocess |
| init не вызван | неверный offset `struct module` | vendor `.ko`/gdb; патч по символу |
| instruction abort на payload PC (`ESR 8600000f`) | NX-страница | `set_memory_x(page,1)` + повторный flush |
| `register_kprobe` unresolved | нет `CONFIG_KPROBES`/kallsyms | метод не применим |
| module signature error | `MODULE_SIG_FORCE=y` | обхода нет; signed/vendor path |
| `CONFIG_MODULES=n` | нет LKM-входа | метод не применим |

---

## Чек-лист адаптации

- [ ] точный running config (`KB_CONFIG`)
- [ ] точный Image + kallsyms
- [ ] vendor `.ko` oracle или QEMU/gdb offsets
- [ ] точный vermagic
- [ ] точный `Module.symvers` или Image+link base
- [ ] CFI mode известен; `KB_CFI` верный
- [ ] `MODVERSIONS` mode известен
- [ ] пути/offsets профиля заполнены
- [ ] builder PASS (финальный gate)
- [ ] QEMU payload gate PASS
- [ ] backup boot + rollback plan
- [ ] установка на устройство только после QEMU; без `rmmod`

## Нельзя

- ставить чужой `.ko`;
- удалять `__versions`, чтобы «загрузилось»;
- переиспользовать CRC/offsets другого ядра;
- глобально отключать CFI или патчить ядро только ради загрузки модуля;
- использовать это для обхода сторонней аутентификации/DRM/CAPTCHA/paywall.
