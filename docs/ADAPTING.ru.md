# Адаптация под другое vendor-ядро — полная инструкция с рефлексией

Это длинная версия. Короткая — `README.ru.md`. Здесь описан **путь пользователя
целиком**: от «подходит ли мой телефон» до `cat /proc/kbootstrap` после
установки. На каждом шаге: зачем, что делать, что должно получиться, что
делать, если не получилось, и какое evidence остаётся.

Builder **не делает универсальный бинарник**. Он берёт факты у target-ядра и
собирает/нормализует модуль под него. Если факта нет — останавливается, а не
угадывает.

---

# Часть I. Карта пути пользователя

```
[0] Подходит ли метод?
      │  CONFIG_MODULES=y? MODULE_SIG_FORCE!=y? KPROBES=y?
      ├─ нет → метод не применим; не трать время
      └─ да
[1] Собрать факты target
      config, kallsyms, Image, vendor .ko, version
[2] Подготовить build tree
      source + O-tree + KB_CONFIG (running config)
[3] Найти ABI oracle
      vermagic, init/exit offsets, CFI mode
[4] Собрать/проверить __versions
      exact Module.symvers или Image+kallsyms+link base
[5] Определить CFI/MODVERSIONS
      KB_CFI, KB_MODVERSIONS
[6] Заполнить профиль
[7] Запустить builder
      build → symtab fix → postprocess → gates → dist
[8] QEMU gate
      insmod_rc=0, payload_result=42
[9] Установка на устройство
      KernelSU zip → reboot → /proc/kbootstrap
[10] Rollback/обслуживание
      reboot, не rmmod; при обновлении ядра — заново
```

Три сценария пользователя:

- **A. То же reference-устройство** (RMX3709 5.10.236): скачал `module.zip` →
  `ksud module install` → reboot → `cat /proc/kbootstrap`. Сборка не нужна.
- **B. Другое устройство, есть vendor `.ko`**: клон → collect → profile →
  build → QEMU → install.
- **C. Другое устройство, vendor `.ko` нет**: то же, но offsets берутся через
  QEMU+gdb; дольше и требует точный Image.

---

# Часть II. Пошагово, с рефлексией

## Шаг 0. Проверить, подходит ли метод

### Зачем
Не начинать сборку, если ядро в принципе не даёт LKM-вход. Это экономит часы.

### Команды

```sh
mkdir -p target
su -c 'zcat /proc/config.gz' > target/config
grep -E '^CONFIG_(MODULES|MODULE_SIG_FORCE|KPROBES|KALLSYMS|KALLSYMS_ALL|MODVERSIONS|CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH|STRICT_KERNEL_RWX|STRICT_MODULE_RWX|MODULE_UNLOAD|MODULE_SCMVERSION)=' target/config
```

### Что означает вывод

| символ | нужно | если нет |
|---|---|---|
| `CONFIG_MODULES=y` | обязателен | метода нет: нет LKM-входа |
| `CONFIG_MODULE_SIG_FORCE` | не `y` | неподписанный модуль не загрузится; обхода нет |
| `CONFIG_KPROBES=y` | обязателен | `kallsyms_lookup_name` этим путём не резолвится |
| `CONFIG_KALLSYMS_ALL=y` | желательно | неэкспортируемые символы могут не найтись |
| `CONFIG_MODVERSIONS=y` | определяет CRC | нужен точный `Module.symvers`/Image |
| `CONFIG_CFI_CLANG=y` | определяет bridge | нужен `KB_CFI=1` |
| `CONFIG_MODULE_UNLOAD=y` | не обязателен | модуль будет «permanent»; всё равно не делай rmmod |

### Рефлексия
- **Зачем:** это gate, а не «пожелание». Без `MODULES=y` любой build — мёртвый код.
- **Если пропустить:** соберёшь `.ko`, потратишь время, получишь «module loading disabled» или silent `-ENOEXEC`.
- **Наблюдаем:** значения в `target/config`.
- **Если не так:** зафиксируй точный символ и остановись; не пытайся обойти подпись/запрет.
- **Evidence:** `target/config`, `grep` output, `uname -a`.

---

## Шаг 1. Собрать факты target

### Зачем
Builder должен знать **точный** вермиagic, ABI и CRC. Приблизительно — нельзя.

### Команды

```sh
tools/collect_target.sh target/
# или вручную:
su -c 'cat /proc/kallsyms' > target/kallsyms
su -c 'cat /proc/version' > target/version
uname -a > target/uname
sha256sum target/config target/kallsyms target/Image 2>/dev/null
```

`Image` достаётся из `boot.img`/`vendor_boot.img` твоими инструментами
(`magiskboot`, `unpack_bootimg`, vendor dumper). Это device-specific;
автоматически не делается.

Vendor `.ko` — любой модуль, который **точно загружается** на устройстве:
`/vendor/lib/modules`, vendor/KernelSU module dir. Скопируй его в `target/`.

### Что проверить

```sh
readelf -p .modinfo target/vendor.ko | grep -E 'vermagic|name='
readelf -SW target/vendor.ko | grep .gnu.linkonce.this_module
readelf -rW target/vendor.ko | sed -n '/this_module/,/^$/p'
readelf -sW target/vendor.ko | grep -E '__cfi|cfi_jt'
```

### Рефлексия
- **Зачем:** Image/kallsyms — источник CRC; vendor `.ko` — ABI-oracle; без них builder не должен угадывать.
- **Если пропустить:** `verify_versions` упадёт; offsets будут неверными; init не выполнится.
- **Наблюдаем:** vermagic, offsets, наличие `__cfi_*`.
- **Если не так:** если vendor `.ko` нет — иди в шаг 3C (QEMU+gdb); если Image нет — нужен точный `Module.symvers` (шаг 4A).
- **Evidence:** hashes, readelf outputs, путь к vendor `.ko`.

---

## Шаг 2. Подготовить build tree

### Зачем
Нужны headers/kbuild **того же kernel family**. Сборка «чем-нибудь» даст несовместимый `.ko`.

### Команды

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

### Почему `KB_CONFIG` отдельно
`olddefconfig` может выкинуть vendor-опции, потому что локальный tree/clang
другие. Поэтому:
- `KB_CONFIG` = **running** config (из `/proc/config.gz`) — используется для решений CFI/MODVERSIONS/KPROBES;
- `KERNEL_OUT/.config` = build config — только для компиляции.

### Рефлексия
- **Зачем:** несовпадение build/running config — частая причина «CFI есть в ядре, но модуль собран без CFI».
- **Если пропустить:** модуль соберётся без bridge/CRC и не загрузится.
- **Наблюдаем:** `modules_prepare` rc=0; наличие `include/generated/autoconf.h`, `Module.symvers`.
- **Если не так:** `KCFLAGS=-Wno-error`; не отключай CFI/LTO глобально.
- **Evidence:** log `modules_prepare`, `grep CONFIG_*` в обоих config.

---

## Шаг 3. ABI oracle: vermagic и `struct module`

### Зачем
Vendor-ядро читает `mod->init`/`mod->exit` по фиксированным offsets. Локальное
дерево может разложить struct иначе.

### 3A. С vendor `.ko`

```sh
readelf -rW target/vendor.ko | sed -n '/this_module/,/^$/p'
# init_module + 0 -> 0x190 (пример reference)
# cleanup_module + 0 -> 0x3c0 (пример reference)
```

### 3B. Без vendor `.ko` — QEMU+gdb

```sh
# загрузить точный Image в QEMU с gdb
# break do_init_module
# inspect mod->init; disassemble do_init_module
```

Передай `--init-off`/`--exit-off` в builder/профиль. Патч делается **по имени
символа**, не по offset.

### Рефлексия
- **Зачем:** неверный offset = kernel читает NULL/мусор → init не вызывается.
- **Если пропустить:** `insmod rc=0`, но кода нет; или panic.
- **Наблюдаем:** relocation offsets; `this_module` size.
- **Если не так:** вернись к vendor `.ko`/gdb; не подставляй «похожий» offset.
- **Evidence:** `readelf -rW`, QEMU gdb log, offsets в profile.

---

## Шаг 4. `__versions` / CRC

### Зачем
`CONFIG_MODVERSIONS=y` + пустая `__versions` может дать silent `-ENOEXEC`
(`CONFIG_MODULE_FORCE_LOAD` не включён).

### 4A. Точный `Module.symvers`
Положи `Module.symvers` из **точной** сборки в `$KERNEL_OUT/Module.symvers`.
Это лучший путь.

### 4B. Восстановление из Image

```sh
KB_REGENERATE_SYMVERS=1 \
KB_LINK_BASE=0x... \
tools/adapt_kernel.sh --profile ...
```

Builder запускает `symvers_from_image.py`, декодируя `__ksymtab/__kcrctab`.
`KB_LINK_BASE` — link-time `_head`/`_text`; возьми из matching `vmlinux`
(`nm vmlinux | grep ' _head\| _text'`) или vendor metadata. Не гадай.

### Рефлексия
- **Зачем:** CRC — не «версия строки», а точный ABI-хэш; неверный CRC = отказ.
- **Если пропустить:** `no symbol version` / `disagrees about version`.
- **Наблюдаем:** `verify_versions.py` PASS, imports count, `crc_bad=0`.
- **Если не так:** не удаляй `__versions`; возьми точный symvers/Image.
- **Evidence:** `Module.symvers` hash/provenance, `verify_versions` output.

---

## Шаг 5. CFI / MODVERSIONS

### Зачем
CFI-ядро вызывает `cfi_init()`, который ищет `__cfi_check`/`__cfi_jt_*`.
Без bridge модуль либо не примет init, либо упадёт на rmmod.

### Команды

```sh
grep -E '^CONFIG_(CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH|MODVERSIONS)=' target/config
```

В профиле:
- `KB_CFI=1` (или `KB_FORCE_CFI=1`) при CFI;
- `KB_MODVERSIONS=1` (или `KB_FORCE_MODVERSIONS=1`) при MODVERSIONS.

### Рефлексия
- **Зачем:** bridge — ABI-мост, не «магия»; он отключает CFI **для модуля**.
- **Если пропустить:** CFI failure / init не вызван.
- **Наблюдаем:** `cfi: not using shadow for module ...` (нормально), затем init.
- **Если не так:** проверь `__cfi_jt_*` в `.ko`; не отключай CFI глобально.
- **Evidence:** config, `readelf` symbols, QEMU log.

---

## Шаг 6. Профиль

### Зачем
Профиль фиксирует **все** target-факты, чтобы сборка была воспроизводимой.

```sh
cp profiles/rmx3709-5.10.236/profile.env profiles/my-device/profile.env
```

```sh
KB_PROFILE_NAME="my-device"
KB_KERNEL_SRC="$HOME/my-kernel"
KB_KERNEL_OUT="$HOME/my-kernel-out"
KB_CONFIG="$HOME/target/config"
KB_IMAGE="$HOME/target/Image"
KB_KALLSYMS="$HOME/target/kallsyms"
KB_VENDOR_KO="$HOME/target/vendor.ko"   # пусто, если используешь offsets
KB_VERMAGIC="<exact>"
KB_INIT_OFF=0x...
KB_EXIT_OFF=0x...
KB_LINK_BASE=0x...                      # только для восстановления symvers
KB_MODULE_CFLAGS=""                     # reference audit off
```

### Рефлексия
- **Зачем:** один файл = воспроизводимость; не надо помнить флаги.
- **Если пропустить:** builder возьмёт build config и ошибётся с CFI.
- **Наблюдаем:** профиль без пустых обязательных полей.
- **Если не так:** `tools/adapt_kernel.sh --help`; проверь каждый путь.
- **Evidence:** профиль (без секретов), `sha256` Image/kallsyms.

---

## Шаг 7. Сборка

```sh
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Что происходит внутри:
1. проверка config gates;
2. выбор CFI/MODVERSIONS;
3. vermagic/offsets (vendor `.ko`/аргументы);
4. symvers (exact или Image);
5. kbuild;
6. `fix_symtab_shndx.py`;
7. `postprocess_common.sh` (versions/modinfo/relocs/checks);
8. `dist/.../{kbootstrap.ko,module.zip,manifest.json}`;
9. QEMU gate (если `--qemu`).

### Рефлексия
- **Зачем:** builder — единственный вход; не собирай вручную «на глаз».
- **Если пропустить:** потеряешь symtab fix/CRC/modinfo.
- **Наблюдаем:** `PASS`, `OK kbootstrap: this_module=0x400 init=0x190 exit=0x3c0`, sha256.
- **Если не так:** читай точную ошибку; не удаляй gate.
- **Evidence:** build log, `manifest.json`, sha256.

---

## Шаг 8. QEMU gate

```sh
KB_IMAGE=$HOME/target/Image \
KB_KO=$PWD/kernel/kbootstrap.ko \
BUSYBOX=$HOME/ae-qemu-gate/bin/busybox \
tests/qemu_payload_gate.sh
```

Проверки:
- `insmod_rc=0`;
- module live;
- `/proc/kbootstrap`;
- `payload_result=42`;
- нет CFI failure.

### Рефлексия
- **Зачем:** device-load — необратимый риск; QEMU ловит ABI/CFI ошибки.
- **Если пропустить:** паника/кирпич на устройстве.
- **Наблюдаем:** `PASS qemu payload gate`.
- **Если не так:** `cfi: not using shadow` — норма; CFI failure — нет; смотри `serial.log`.
- **Evidence:** QEMU log, KO sha.

---

## Шаг 9. Устройство

```sh
# backup boot/vendor_boot!
ksud module install dist/<profile>/module.zip
# reboot
cat /proc/kbootstrap
```

### Рефлексия
- **Зачем:** KernelSU-модуль — доставка; reboot — активация.
- **Если пропустить backup:** нет rollback.
- **Наблюдаем:** `/proc/kbootstrap` с `payload_result=42`.
- **Если не так:** reboot; не rmmod; смотри `load.log`/dmesg; вернись к QEMU.
- **Evidence:** dmesg, proc output, module zip sha.

---

## Шаг 10. Rollback/обслуживание

- Rollback = reboot без модуля/с восстановленным boot.
- **Не делай `rmmod`** до доказанного unload/CFI.
- При обновлении ядра: заново собрать профиль (vermagic/CRC/offsets меняются).
- Не переиспользуй `.ko` между ядрами.

---

# Часть III. Сценарии пользователя

## A. Reference-устройство
1. Скачать release `module.zip`.
2. `ksud module install module.zip`.
3. Reboot.
4. `cat /proc/kbootstrap`.
5. Если нет — `load.log`, dmesg; проверить, что ядро именно reference.

## B. Другое устройство + vendor `.ko`
1. `collect_target`.
2. Build tree + `KB_CONFIG`.
3. `readelf` vendor `.ko` → vermagic/offsets.
4. Exact `Module.symvers` или Image+link base.
5. Profile.
6. `adapt_kernel.sh --qemu`.
7. Install.

## C. Другое устройство без vendor `.ko`
1–2 как B.
3. QEMU+gdb: `do_init_module`, `mod->init` offset.
4. `--init-off/--exit-off`.
5. Далее B.

## D. CVE-патч
1. Upstream fix commit → функция/условие.
2. Маппинг на vendor symbol.
3. Kprobe guard/trampoline.
4. Build.
5. QEMU с репродьюсером.
6. Install.
Подробно: `docs/CVE-PATCHING.ru.md`.

---

# Часть IV. Troubleshooting с рефлексией

| симптом | почему | что делать | чем проверить |
|---|---|---|---|
| `no symbol version` / silent `-ENOEXEC` | нет/пустая `__versions`; force-load off | exact symvers/Image | `verify_versions.py` PASS |
| `disagrees about version` | CRC не от этого ядра | target Image/kallsyms | crc_bad=0 |
| `no version magic` | vermagic неверный | vendor `.ko`/`/proc/version` | `readelf -p .modinfo` |
| CFI failure | нет/неверный bridge | `KB_CFI=1`, bridge objects | `__cfi_jt_*` |
| Oops `load_module+0x20d4` | `SHN_XINDEX` без таблицы | `fix_symtab_shndx.py` | symtab fixed/N |
| init не вызван | offset `struct module` | vendor `.ko`/gdb | gdb `mod->init` |
| IABT payload (`8600000f`) | NX страница | `set_memory_x` + flush | `set_memory_x_rc=0` |
| `register_kprobe` unresolved | нет KPROBES/kallsyms | метод не применим | config |
| signature error | `SIG_FORCE` | нет обхода | config |

---

# Часть V. Чек-лист и запреты

- [ ] `MODULES=y`, `SIG_FORCE!=y`, `KPROBES=y`
- [ ] config/kallsyms/Image/vendor `.ko` собраны
- [ ] build tree + `KB_CONFIG`
- [ ] vermagic/offsets
- [ ] exact symvers или Image+link base
- [ ] `KB_CFI`/`KB_MODVERSIONS`
- [ ] профиль заполнен
- [ ] builder PASS
- [ ] QEMU PASS
- [ ] backup/rollback
- [ ] install; no rmmod

**Нельзя:** чужой `.ko`; удалять `__versions`; переиспользовать CRC/offsets;
глобально отключать CFI; обходить аутентификацию/DRM/CAPTCHA/paywall.
