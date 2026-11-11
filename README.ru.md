<p align="center">
  <img src="assets/banner.svg" alt="Vendor Kernel Runtime Entry — библиотека runtime-модулей для замороженных вендорских ядер" width="100%">
</p>

<p align="center">
  <a href="https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/latest"><img alt="release" src="https://img.shields.io/github/v/release/kosoymiki/vendor-kernel-runtime-entry?style=flat-square&color=22d3ee&label=release"></a>
  <a href="https://github.com/kosoymiki/vendor-kernel-runtime-entry/actions/workflows/static.yml"><img alt="ci" src="https://img.shields.io/github/actions/workflow/status/kosoymiki/vendor-kernel-runtime-entry/static.yml?style=flat-square&label=ci"></a>
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0--only-a78bfa?style=flat-square">
  <img alt="platform" src="https://img.shields.io/badge/platform-Android%2016%20%C2%B7%20arm64-60a5fa?style=flat-square">
  <img alt="kernel" src="https://img.shields.io/badge/kernel-5.10.236%20vendor-34d399?style=flat-square">
  <img alt="status" src="https://img.shields.io/badge/status-active%20development-fbbf24?style=flat-square">
</p>

<p align="center">
  <a href="README.md">English</a> ·
  <b>Русский</b> ·
  <a href="docs/ADAPTING.ru.md">Адаптация</a> ·
  <a href="docs/ENTRYPOINTS.ru.md">Матрица входа</a> ·
  <a href="docs/MGLRU.ru.md">MGLRU</a> ·
  <a href="docs/CVE-PATCHING.ru.md">CVE-патчи</a>
</p>

---

# Runtime Entry для вендорского ядра Linux

> ## 🚧 Активная разработка
>
> **Это живая библиотека, а не замороженный снимок.** Библиотека модулей,
> per-target builder и методика адаптации пополняются **регулярно**: новые
> бэкпортированные модули, новые механизмы входа, новые профили устройств и
> новые evidence появляются по мере того, как проходят QEMU-гейты. Интерфейсы,
> оффсеты и упаковка модулей могут меняться между минорными версиями.
>
> Каждое утверждение в репозитории привязано к воспроизводимому логу. Если
> стадия ещё не измерена — она помечена как *planned* или *in progress*, но
> никогда как «сделано».

---

## Содержание

- [Что это](#что-это)
- [Почему это работает](#почему-это-работает)
- [Архитектура](#архитектура)
- [Матрица точек входа](#матрица-точек-входа)
- [Библиотека модулей](#библиотека-модулей)
- [Быстрый старт](#быстрый-старт)
- [Адаптация под своё устройство](#адаптация-под-своё-устройство)
- [Evidence](#evidence)
- [Дорожная карта](#дорожная-карта)
- [Версии](#версии)
- [Документация](#документация)
- [Модель безопасности и границы](#модель-безопасности-и-границы)
- [Структура репозитория](#структура-репозитория)
- [Участие](#участие)
- [Источники ресерча](#источники-ресерча)
- [Лицензия](#лицензия)

## Что это

**Runtime-вход** для замороженных вендорских ядер Android плюс **растущая
библиотека модулей**, построенных на нём.

Возможность ядра — это compile-time переключатель. Если вендор собрал
`CONFIG_KEXEC=n`, `CONFIG_DEVMEM=n`, без `LIVEPATCH` и `FUNCTION_TRACER`, а
загрузчик заблокирован — штатного способа добавить функциональность ядра в
рантайме нет. Единственная дверь, которую вендоры обычно оставляют открытой —
**загружаемые модули**.

Репозиторий проходит через эту дверь, доказывает, что за ней можно делать, и
упаковывает результат для **KernelSU / ReSukiSU** на **Realme GT3 (RMX3709)** с
вендорским ядром `5.10.236-android12-9-o-g27a48ef6c082`
(`CFI_CLANG=y`, `LTO_CLANG=y`, SCS/BTI/PAC, strict W^X).

Это **референсная реализация + per-target builder**, а не универсальный бинарь.
Метод описан пошагово, чтобы под него можно было адаптировать другое
заблокированное ядро — см. [Адаптация](#адаптация-под-своё-устройство).

## Почему это работает

Между «файлом `.ko`» и «кодом, который реально исполняется» стоят пять проблем:

1. **`vermagic` и `.modinfo`** должны точно совпадать с работающим ядром.
2. **`__versions` / CRC** — вендорское ядро собрано с `MODVERSIONS`, поэтому
   каждому импортируемому символу нужен реальный CRC из *его* `Image`, а не из
   публичного дерева.
3. **Раскладка `struct module`** — `init`/`exit` релоцируются по вендорским
   оффсетам (`0x190` / `0x3c0` на референсе), а не по upstream 5.10.
4. **CFI** — `cfi_init()` проверяет CFI-таблицы модуля ещё до вызова
   `init_module`. Модуль, собранный clang 21 без LTO, не может случайно
   удовлетворить ядро clang 12 с LTO.
5. **`SHN_XINDEX`** — `ld.lld` выдаёт индекс таблицы символов, который
   вендорский загрузчик не принимает, пока не исправлен индекс в section header.

CFI-мост — самая интересная часть. `__cfi_check` немедленно возвращает,
`__cfi_jt_init_module` / `__cfi_jt_cleanup_module` указывают на
`bti c; b init_module`, а раскладка `.ref.text`/`.ref.data` совпадает с тем, что
обходит `cfi_init()` (`kernel/module.c:4530-4545`). Мост **не** отключает CFI
глобально — он отключает проверки **для этого модуля**. Значит, код модуля по
построению является доверенным, и репозиторий именно так к нему и относится.

## Архитектура

<p align="center">
  <img src="assets/architecture.svg" alt="Слои: boot chain, совместимость загрузчика, CFI-мост, runtime-вход, библиотека модулей" width="100%">
</p>

Ничего выше моста не является kernel-wide. Загрузчик принимает один чужой
модуль; мост делает этот модуль вызываемым; механизмы входа
(kprobe/kretprobe, text patch, трамплин, raw payload, procfs/sysfs) делают
работу; библиотека модулей это упаковывает.

## Матрица точек входа

<p align="center">
  <img src="assets/entry-matrix.svg" alt="Матрица точек входа: доказано, только observe, закрыто" width="100%">
</p>

| механизм | требует | статус | что даёт |
|---|---|---|---|
| LKM + CFI bridge | `MODULES=y`, unsigned разрешены | **доказано** | init/exit и callbacks модуля |
| kprobe / kretprobe | `KPROBES=y` | **доказано** | резолв неэкспортируемых символов, правка аргументов |
| text patch | патчер + W^X | **доказано (QEMU, 1 CPU)** | замена живой функции + restore |
| трамплин | патчер + `module_alloc` | **доказано (QEMU, 1 CPU)** | замена тела функции целиком |
| raw payload | `module_alloc`, `set_memory_x` | **доказано** | исполнение доверенного AArch64 |
| kallsyms через kprobe | `KPROBES`, kallsyms | **доказано** | резолв неэкспортируемых символов |
| procfs / sysfs | `proc_create` | **доказано** | статус и управление |
| BPF | `BPF_SYSCALL/EVENTS/JIT` | только observe | tracing, ограничения верификатора |
| ftrace / livepatch / kexec / devmem | их конфиги | **закрыто** | отсутствуют в вендорском конфиге |

Подробности, включая то, чего text patch **не** доказывает:
[`docs/ENTRYPOINTS.ru.md`](docs/ENTRYPOINTS.ru.md) ([EN](docs/ENTRYPOINTS.md)).

## Библиотека модулей

<p align="center">
  <img src="assets/library.svg" alt="Библиотека модулей: kbootstrap, kpatch, ktramp, mglru_probe" width="100%">
</p>

| модуль | версия | статус | что доказывает | evidence |
|---|---|---|---|---|
| [`kbootstrap`](kernel/) | 1.0.0 | stable probe | CFI-мост, резолв kallsyms, raw payload, `/proc` | `payload_result=42` · `065f520b…` |
| [`kpatch`](modules/kpatch/) | 1.0.2 | probe | save → `aarch64_insn_patch_text_nosync` → проверка → restore | `getpid 114 → 0 → 118` · `c4f9a319…` |
| [`ktramp`](modules/ktramp/) | 1.2.0 | экспериментальный | Stage F M4 subset: per-memcg часы scan-эпох (`seq`/`win`), young/cold, порог refault по окну | `seq=125`, `win=2`, ветка 7 cold, `y80` 50.6%, `t5` 100%, `tdist=847` · `34bf93d0…` |
| [`mglru_probe`](modules/mglru/) | 1.0.5 | экспериментальный | kprobe на reclaim path + политика приоритета `shrink_node`, чистый rmmod | `changed=55/84`, `gt0=55` · `9a142d8b…` |

Смысл проекта — именно библиотека: каждый модуль собирается из этого дерева
одним builder'ом, проходит QEMU-гейт и поставляется вместе со своим evidence.
Новые бэкпорты (vmscan, workingset, folio) добавляются по мере прохождения
гейтов.

## Быстрый старт

**Только референсная цель** (RMX3709 / 5.10.236). Чужой `.ko` не загрузится, а
попытка его протолкнуть — это способ окирпичить устройство.

```sh
# KernelSU / ReSukiSU
ksud module install module.zip
# reboot, затем:
cat /proc/kbootstrap
```

Сборка из исходников:

```sh
git clone https://github.com/kosoymiki/vendor-kernel-runtime-entry.git
cd vendor-kernel-runtime-entry

# базовый модуль
KB_MODULE_SRC=$PWD/kernel KB_MODNAME=kbootstrap \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

# probe-модули
KB_MODULE_SRC=$PWD/modules/ktramp KB_MODNAME=ktramp \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
```

Каждая сборка прогоняет ELF/ABI/CRC-гейты и печатает `PASS` по артефакту:

```
ktramp.ko   entries=5  imports=4  crc_bad=0  not_exported=0
OK ktramp: this_module=000400 init=0000000000000190 exit=00000000000003c0
sha256 bbfb599a17db566dae1eb664dbafba5de1f5568bd08ac7363ca1ceefc35b55e3
```

## Адаптация под своё устройство

Нужны четыре входа: `Image` цели, её `kallsyms`, один вендорский `.ko` для формы
ABI и конфиг цели. Builder отказывается угадывать.

```sh
tools/collect_target.sh target/
# заполнить profiles/my-device/profile.env: четыре входа + оффсеты
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Полная цепочка — распаковка boot image, kallsyms, генерация CRC, оффсеты
`struct module`, определение CFI, QEMU-гейт, упаковка KernelSU, разбор ошибок —
в [`docs/ADAPTING.ru.md`](docs/ADAPTING.ru.md) ([EN](docs/ADAPTING.md)).
Каждый шаг описан вместе с причиной и режимами отказа.

## Evidence

<p align="center">
  <img src="assets/evidence.svg" alt="Сокращённый serial-лог QEMU для трамплина" width="100%">
</p>

| артефакт | sha256 | гейт |
|---|---|---|
| `kbootstrap.ko` | `065f520b1b562f55eef48a138a12d4df3c18d51357bcbacb39b46d1e9a33c593` | `tests/qemu_payload_gate.sh` |
| `kpatch.ko` | `c4f9a319feb6da9d8989bbdb15903bcb6ccce4c361c1fcd1edfc237dfe5ddf11` | `tests/qemu_probe_gate.sh` |
| `ktramp.ko` | `34bf93d01b2a37b88509f7cdeb316a245c705afc6de0cfe92d48fb6be2f21100` | `tests/qemu_ktramp_gate.sh` |
| `mglru_probe.ko` | `9a142d8b04c03a343839d38a935ad84c06f8e6e76f629d4d39fbf8a11dd53d35` | `tests/qemu_mglru_gate.sh` |

Измерено в QEMU на точном стоковом `Image` (`-smp 1`, `maxcpus=1`):

- **вход:** `cfi: not using shadow for module kbootstrap` →
  `Kernel panic - not syncing: kbootstrap entered`, в трейсе
  `init_module+0x1c/0xfe8 [kbootstrap]` — `init_module` действительно выполнился.
- **text patch:** первые инструкции `__arm64_sys_getpid` сохранены, записан
  `mov w0,#0; ret`, `getpid` вернул `0`, после restore — `118`.
- **трамплин (Stage F):** вход `get_scan_count` заменён ветвлением в обёртку
  модуля; per-memcg часы scan-эпох `seq=125`, `win=2`; холодный кэш — ветка 7
  (полный скан), форс young `y80` — ветка 6 (`delta20=591/1167` = 50.6%),
  форс distance `t5` — ветка 1 (100%), реальный refault — ветка 2
  (`tdist=847`); `patched=0`, `rmmod_rc=0`, `proc_gone`, `module_gone`.
- **reclaim-probes:** baseline `get_scan_count=54`, `pgscan_file=1428`; mode 5
  `get_scan_count=84`, `pgscan_file=4478`, `changed=55/phits=84`
  (`gt0=55`, `zero=84`). Накопленные суммы несравнимы между прогонами —
  fixed-endpoint метрика `delta/pre` в `ktramp` 1.1.0; см.
  [`docs/MGLRU-STAGE-E.ru.md`](docs/MGLRU-STAGE-E.ru.md).
- **payload:** `/proc/kbootstrap` отдаёт `payload_result=42`.

Как именно создаётся reclaim-нагрузка (и почему `tmpfs` не работает) —
[`docs/MEASURING-RECLAIM.md`](docs/MEASURING-RECLAIM.md).

## Дорожная карта

<p align="center">
  <img src="assets/roadmap.svg" alt="Дорожная карта: вход, text patch, трамплин, policy hooks, бэкпорт 7.4, parity" width="100%">
</p>

| стадия | состояние | содержание |
|---|---|---|
| M0 вход | **готово** | LKM + CFI-мост + raw payload |
| M1 text patch | **готово** | save / patch / verify / restore |
| M2 трамплин | **готово** | замена функции целиком |
| M3 policy hooks | **готово** | Stage E поколенческая политика, shadow refault distance, SMP-safe патчинг, restore/unload |
| M4 бэкпорт 7.4 | **готово** | Stage F: per-memcg часы scan-эпох (`seq`/`win`), порог refault по окну, young/cold по эпохе |
| M5 parity | план | бенчмарки, gap-report, parity-subset |

Probe — это не бэкпорт. M3/M4 закрыты с поведенческим evidence и явным gap-list;
QEMU и явным списком пробелов; дельта 5.10 → 7.4 измерена в
[`docs/BACKPORT-7.4.md`](docs/BACKPORT-7.4.md).

## Версии

| тег | что вошло |
|---|---|
| [v1.0.0](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.0) | vendor-ABI LKM, CFI-мост, payload bus, пакет KernelSU |
| [v1.0.1](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.1) | двуязычная документация, полные цепочки адаптации и CVE |
| [v1.0.2](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.2) | `kpatch` (text patch), `mglru_probe` Stage A/B, матрица входа |
| [v1.0.3](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.3) | `ktramp` (трамплин), real-reclaim гейт, SVG-инфографика, новый README |
| [v1.0.4](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.4) | `mglru_probe` mode 5 (политика приоритета), A/B reclaim-гейт, измеренные числа стадии |
| [v1.1.0](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.1.0) | MGLRU Stage E: поколенческая young/cold-политика, shadow refault distance, `delta/pre`-гейт, restore/unload, `mglru_probe` 1.0.5 |
| [v1.2.0](https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.2.0) | MGLRU Stage F (M4 subset): per-memcg часы scan-эпох `seq`/`win`, порог refault по окну, 31/31 unit-тестов |

## Документация

| документ | RU | EN | что внутри |
|---|---|---|---|
| Гайд по адаптации | [ADAPTING.ru](docs/ADAPTING.ru.md) | [ADAPTING](docs/ADAPTING.md) | полная цепочка, рефлексия по шагам, разбор ошибок |
| Матрица входа | [ENTRYPOINTS.ru](docs/ENTRYPOINTS.ru.md) | [ENTRYPOINTS](docs/ENTRYPOINTS.md) | каждый механизм с evidence и границами |
| Измерение reclaim | — | [MEASURING-RECLAIM](docs/MEASURING-RECLAIM.md) | как заставить QEMU реально сканировать file LRU |
| MGLRU probe | [MGLRU.ru](docs/MGLRU.ru.md) | [MGLRU](docs/MGLRU.md) | статус Stage A/B, что **не** является parity |
| CVE-патчи | [CVE-PATCHING.ru](docs/CVE-PATCHING.ru.md) | [CVE-PATCHING](docs/CVE-PATCHING.md) | шаблон defensive runtime-митигации |
| Методология | — | [METHODOLOGY](docs/METHODOLOGY.md) | как анализируется цель и что гейтится |
| Безопасность | — | [SECURITY](docs/SECURITY.md) | модель угроз, граница доверия, режимы отказа |
| Карта бэкпорта 7.4 | — | [BACKPORT-7.4](docs/BACKPORT-7.4.md) | измеренная дельта, syscalls, порядок фич |
| Процедура релиза | — | [RELEASE](docs/RELEASE.md) | бамп версии по языкам, докам и графике |

## Модель безопасности и границы

Это **произвольное исполнение кода в ядре**. Неверный модуль может вызвать
panic, повредить память или окирпичить устройство. Репозиторий построен вокруг
этого факта:

- **Сначала QEMU.** Каждый модуль проходит гейт на точном стоковом `Image`,
  прежде чем о нём вообще говорят, что он работает.
- **Никакого `rmmod` до restore.** Модуль, пропатчивший текст, обязан вернуть
  оригинал, и restore должен быть наблюдён, прежде чем обсуждать выгрузку.
- **`nosync` — только один CPU.** `aarch64_insn_patch_text_nosync` не
  останавливает другие CPU. На SMP нужен `aarch64_insn_patch_text`/`stop_machine`
  и проверка под нагрузкой.
- **CFI-мост локален для модуля.** Проверки отключены для модуля, не для ядра.
  Код модуля — доверенный; подписывайте и ревьюйте его соответственно.
- **Универсального бинаря нет.** Профили per-target; несовпавший `.ko`
  отбрасывается by design.

Перед загрузкой на устройство прочитайте [`docs/SECURITY.md`](docs/SECURITY.md).

## Структура репозитория

```
assets/      сгенерированная SVG-инфографика (баннер, архитектура, матрица, roadmap, evidence)
kernel/      исходник базового модуля + CFI-мост
modules/     kpatch (text patch), ktramp (трамплин), mglru (probe Stage A/B)
module/      пакет KernelSU / ReSukiSU
tools/       per-target builder, ELF/ABI/CRC-гейты, delta и генератор ассетов
profiles/    профили целей (референс: RMX3709 5.10.236)
tests/       статические проверки, QEMU-гейты, freestanding helper для reclaim
docs/        адаптация, матрица входа, reclaim, MGLRU, CVE, методология, безопасность
```

Инфографика генерируется, а не рисуется руками: `python3 tools/make_assets.py`
детерминированно пересобирает все шесть SVG и валидирует XML и геометрию.
Меняйте генератор, а не ассет.

## Участие

- Держите evidence рядом с изменением: модуль без QEMU-гейта не мержится.
- Один логический change на коммит; документация и код едут вместе.
- Новые цели — в `profiles/<device>-<kernel>/profile.env` с реально
  наблюдёнными входами и оффсетами, без догадок.
- Probe обязан называться probe: `probe`, `experimental` или `stable probe`.
  «Backported» зарезервировано для измеренной parity.
- Перед pull request: `python3 tools/check_versions.py && bash tests/static_checks.sh`.
  Бамп версии, который пропустил язык, таблицу или сгенерированный ассет,
  падает на этом гейте by design — см. [`docs/RELEASE.md`](docs/RELEASE.md).

## Источники ресерча

Структура README и документации опирается на первичные источники, accessed
2026-11-10 через headless browser (stealth-browser MCP):

| источник | для чего использован |
|---|---|
| [GitHub Docs — About the repository README file](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/about-readmes) | что ожидается от README |
| [GitHub Docs — basic writing and formatting](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax) | Markdown, изображения, HTML-блоки |
| [Best-README-Template](https://github.com/othneildrew/Best-README-Template) | порядок секций, TOC, строка бейджей |
| [awesome-readme](https://github.com/matiassingers/awesome-readme) | критерии оценки (баннер, бейджи, TOC, скриншоты, шаги установки) |
| [shields.io](https://shields.io/) | синтаксис статических и динамических бейджей |
| [Mermaid docs](https://mermaid.js.org/syntax/flowchart.html) | конвенции диаграмм (использован SVG — ради офлайн-детерминизма) |

## Лицензия

GPL-2.0-only. Модули ядра должны быть GPL-совместимыми.
