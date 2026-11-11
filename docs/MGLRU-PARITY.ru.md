# Отчёт о parity MGLRU — runtime-backport против 7.4

Область: что из 7.4 MGLRU воспроизведено runtime-модулями (`ktramp`,
`mglru_probe`, `kpatch`, `kbootstrap`) на vendor-ядре 5.10.236, что
приблизительно и что нельзя воспроизвести без пересборки vmscan.

Метод: каждая строка сравнивает механизм 7.4 (исходники: `mm/vmscan.c`,
`mm/workingset.c`, `Documentation/admin-guide/mm/multigen_lru.rst`) с кодом в
`modules/ktramp/` и выводом QEMU-гейта. Статусы основаны на evidence, а не на
заявлениях.

Легенда: **exact** — тот же алгоритм/данные; **approximate** — то же решение
на runtime-замене; **partial** — только часть механизма; **absent as
structure** — структура не заменяется (side table вместо неё).

| механизм | 7.4 | runtime-реализация | статус |
|---|---|---|---|
| per-memcg состояние поколений | `lru_gen_folio` с `min_seq/max_seq` | `mgseq/mgwin` на memcg из `android_vh_shrink_node_memcgs` | approximate |
| часы поколений | двигаются при создании поколения | двигаются на скан-эпоху memcg; `window=min(seq,2)` | approximate |
| хранение поколения страницы | folio flags / списки поколений | side table `page -> seq` (8192 записей, hash) | approximate |
| young/cold | принадлежность спискам поколений | `seq` страницы равен текущей эпохе → young, иначе cold | approximate |
| гистограмма возраста вытеснений | generation stats | 4-bucket `ev_age` (0,1,2,3+) на memcg | approximate |
| refault distance | shadow timestamp против `nonresident_age` | идентично: `shadow >> 19`, `lruvec+96`, 46-битная маска | **exact** |
| порог refault | сравнение с `max_seq - min_seq` | `32 * window`, окно масштабируется адаптивным ttl | approximate |
| адаптивный `min_ttl` | `min_ttl_ms` в миллисекундах | ttl 2..4 из отношения refault/eviction | approximate |
| per-generation LRU-списки | списки `lru_gen` | модуль сортирует inactive-списки ядра в 4 возрастных bucket'а, oldest-first, под `lru_lock`; структур `lru_gen` нет | approximate (реальный порядок списков) |
| folio-пути | multi-order folios | нормализация head-страниц в таблицах модуля; QEMU не производит compound-страниц на LRU (r2-доказанный vendor-гейт THP/CHP init) | partial (unit-tested) |
| патч текста / SMP | n/a | `aarch64_insn_patch_text` (stop_machine), restore | **exact** |
| вход скана per-memcg (`shrink_node_memcgs`) | прямой вызов | трамплин модуля на входе; priority boost; перехват per-node/memcg | **exact entry point** |
| кастомный `shrink_lruvec` (вход vmscan) | пересборка исходников | `vmscan_hook`: функция модуля заменяет `shrink_lruvec`, вызывает штатные `shrink_active_list`/`shrink_inactive_list` | **exact entry point**, приблизительный цикл |
| наблюдаемость | tracepoints | kprobes + vendor-tracepoint; счётчики в `/proc` | **exact** |

## Что доказывают гейты

- **Stage A/B:** kprobe на reclaim path регистрируются 6/6 (`reg=1 err=0`) и
  срабатывают под реальным reclaim; mode 5 переписывает
  `scan_control.priority` (`changed=55/84`, `gt0=55`, `zero=84`).
- **Stage E/F:** per-memcg поколения и часы scan-эпох; холодный кэш уходит в
  ветку полного скана; `y80` защищает рабочий набор (~50.6%); `t5` обнуляет
  file-сканы (100%); реальный refault делит их пополам (`tdist=847`);
  restore и unload чистые.
- **Stage G:** адаптивный ttl 2..4 из thrash, 4-bucket возраст вытеснений,
  ветки 8/9 (старые вытеснения → полный скан, молодые → защита).

## Явный gap-list

1. **Нет `lru_gen`-списков.** Сами структуры списков не заменяются; runtime
   использует side table плюс кастомный `shrink_lruvec` (`vmscan_hook`),
   который сортирует inactive-списки ядра в возрастные bucket'ы (oldest-first)
   и вызывает штатные сканеры. Порядок MGLRU-подобный; хранилище — по-прежнему
   списки ядра, а не `lru_gen`.
2. **Семантика часов.** Эпоха двигается на скан memcg (vendor-tracepoint), а не
   в точной точке создания поколения MGLRU. Это меняет границы поколений, но
   не направление решения.
3. **Масштабирование порога refault.** MGLRU сравнивает последовательности
   поколений; runtime сравнивает счётчики страниц и масштабирует окном. Сам
   distance точен; порог приблизителен.
4. **`min_ttl` — не миллисекунды.** Runtime ttl — окно 2..4, управляемое
   отношением refault/eviction, а не wall-clock TTL.
5. **Folio tails.** `isolate_lru_pages` отбрасывает tail-страницы
   (`mm/vmscan.c:1953`), поэтому probe вытеснения/refault всегда получают head;
   нормализация tail→head покрыта unit-тестом, но не QEMU.
6. **Без пересборки исходников.** Модули хукают/заменяют функции (`kpatch`,
   `ktramp`, kprobes) и не меняют образ ядра; полный source-backport MGLRU
   остаётся задачей сборки ядра, а не LKM.

## Карта evidence

| стадия | документ | гейт |
|---|---|---|
| A/B | `docs/MGLRU.ru.md` | `tests/qemu_mglru_gate.sh` |
| C/D | `docs/MGLRU-STAGE-E.ru.md` | `tests/qemu_ktramp_gate.sh` |
| E | `docs/MGLRU-STAGE-E.ru.md` | `tests/qemu_ktramp_gate.sh` |
| F | `docs/MGLRU-STAGE-F.ru.md` | `tests/qemu_ktramp_gate.sh` |
| G | `docs/MGLRU-STAGE-G.ru.md` | `tests/qemu_ktramp_gate.sh` |
| вход vmscan | `docs/VMSSCAN-HOOK.ru.md` | `tests/qemu_vmscan_hook_gate.sh` |

Дельта файлов/LOC 5.10 → 7.4 и матрица фич — в
[`BACKPORT-7.4.ru.md`](BACKPORT-7.4.ru.md).
