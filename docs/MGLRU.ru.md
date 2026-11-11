# MGLRU probe: статус Stage A/B/E/F (экспериментально)

`modules/mglru` — **probe**, не полный backport 7.4 MGLRU. Он измеряет reclaim
path 5.10 и проверяет generation-aware политики через доказанный entry.

## Что делает

- **Stage A (read-only):** kprobes на `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`; счётчики в `/proc/mglru_probe`.
- **Stage B (политика), выбор цифрой в `/proc/mglru_probe`:**
  - `1` anon-light, `2` file-light, `3` aggressive — эвристики по `nr[]`;
  - `4` generation-aware — решение на входе `shrink_node` по side table
    поколений страниц;
  - `5` priority policy — на входе `shrink_node` выставляет
    `scan_control.priority` в 0 (агрессивный reclaim).
- **Stage E (поколенческая политика, `ktramp` 1.1.0):** `mark_page_accessed`
  сохраняет поколения страниц, вытеснение классифицирует young/cold, а
  трамплин `get_scan_count` защищает молодой рабочий набор или оставляет
  полный скан, когда доминируют холодные страницы. Shadow-refault distance и
  per-memcg атрибуция — в том же модуле. Детали:
  [`MGLRU-STAGE-E.ru.md`](MGLRU-STAGE-E.ru.md).
- **Stage F (M4 subset, `ktramp` 1.2.0):** per-memcg часы scan-эпох из
  vendor-tracepoint (`seq`, `window=min(seq,2)`), порог refault по окну
  (`32*window`), young/cold по эпохе. Детали:
  [`MGLRU-STAGE-F.ru.md`](MGLRU-STAGE-F.ru.md).
- kprobe на `mark_page_accessed` заполняет side table поколений (1024 записи).

Mode 5 берёт `scan_control` из `regs[1]` и пишет поле `s8 priority` по оффсету
`0x2c` (оффсет проверен по ABI цели, см. `docs/OFFSETS.md` соседней сессии).
Считаются все входы (`phits`) и только те записи, что реально изменили значение
(`changed`).

## Дефект, который стоит помнить: kprobe + kretprobe на одном символе

В ранней ревизии на `get_scan_count` регистрировались и kprobe, и kretprobe.
Return-handler не срабатывал (`modified=0`, `nmissed=0`): kprobe на том же
адресе подавляет kretprobe-путь. Kretprobe убран; решение по политике теперь
принимается на входе `shrink_node`, где `scan_control` и так доступен.

## Наблюдения в QEMU (exact stock Image)

Нагрузка: ext4-файл через `loop0` + 345 MiB анонимного давления, поллинг
`pgscan_file` — см. [`MEASURING-RECLAIM.md`](MEASURING-RECLAIM.md). Гейт:
`tests/qemu_mglru_gate.sh`.

| метрика | mode 0 (baseline) | mode 5 (priority policy) |
|---|---|---|
| `get_scan_count` hits | 54 | 84 |
| `page_referenced` hits | 1428 | 3977 |
| `pgscan_file` | 1428 | 4478 |
| `pgscan_kswapd` | 930 | 4198 |
| `pgscan_direct` | 498 | 280 |
| записей политики | — | `changed=55` / `phits=84` (`gt0=55`, `zero=84`) |

**Что доказано:** хук срабатывает и переписывает `scan_control.priority` в 55
из 84 вызовов `shrink_node` (`gt0=55`, `zero=84`). Это детерминированный факт
в ядре.

**Почему накопленный `pgscan_file` — не метрика:** это running total, а
нагрузка обрывается OOM-killer'ом в разной точке; пара независимых прогонов
дала `4178` (mode 0) против `6834` (mode 5) — противоположное направление.
Суммы между прогонами несравнимы. `ktramp` 1.1.0 закрывает это fixed-endpoint
метрикой `delta/pre` на 20-м и 40-м вызовах `get_scan_count`.

## Почему это не «MGLRU backported»

- В 5.10 нет `gen_lru`; используется маленькая side table (hash по page), а не
  структуры `lru_gen` 7.4.
- Захукано несколько функций; 7.4 MGLRU охватывает `mm/vmscan.c`,
  `mm/workingset.c`, page flags, lruvec и многое другое.
- Запись `sc->priority` — грубый инструмент, а не generation-based reclaim.

## Следующие стадии

- **Stage C/D/E уже в `ktramp` 1.1.0:** full-function трамплин с SMP-safe
  патчингом, per-memcg таблицы поколений, shadow-refault distance,
  young/cold-тиринг, fixed per-call `delta/pre`, restore и unload. См.
  [`MGLRU-STAGE-E.ru.md`](MGLRU-STAGE-E.ru.md).
- **M4 (сделан как Stage F):** per-memcg часы scan-эпох, порог refault по
  окну, young/cold по эпохе. См. [`MGLRU-STAGE-F.ru.md`](MGLRU-STAGE-F.ru.md).
- **M5 (дальше):** полный parity-report: адаптивный `min_ttl`,
  per-generation LRU-списки, folio-пути и явный gap-list точности.

## Как правильно измерять reclaim

`tmpfs` без swap не запускает сканер, чтение `brd` здесь не наполняет file LRU,
а обычные аллокации молча берутся из page cache. Рабочая методика — ext4-файл
через `loop0` плюс анонимное давление с поллингом `pgscan_file` — в
[`MEASURING-RECLAIM.md`](MEASURING-RECLAIM.md).

## Сборка и гейт

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

MODE=0 KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/mglru/mglru_probe.ko bash tests/qemu_mglru_gate.sh
MODE=5 KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/mglru/mglru_probe.ko bash tests/qemu_mglru_gate.sh
```

Эксперимент: сначала QEMU; отключить политику (`write 0`) перед выгрузкой; без
`rmmod` на устройстве, пока unload-path не доказан.
