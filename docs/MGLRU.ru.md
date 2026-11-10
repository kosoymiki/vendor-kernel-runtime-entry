# MGLRU probe: статус Stage A/B (экспериментально)

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
| `shrink_node` hits | 101 | 102 |
| `get_scan_count` hits | 101 | 102 |
| `page_referenced` hits | 5266 | 4436 |
| `pgscan_file` | 5934 | 4880 |
| `pgscan_kswapd` | 5768 | 4655 |
| `pgscan_direct` | 166 | 225 |
| записей политики | — | `changed=77` / `phits=102` |

**Что доказано:** хук срабатывает и переписывает `scan_control.priority` в 77
из 102 вызовов `shrink_node`. Это детерминированный факт в ядре.

**Что не доказано:** что политика меняет *объём* сканирования. `pgscan_file` —
накопленный счётчик, а нагрузка обрывается OOM-killer'ом в разной точке: пара
независимых прогонов того же гейта дала `4178` (mode 0) против `6834` (mode 5),
то есть противоположное направление. Суммы между прогонами несравнимы; вопрос
закрывается только fixed-endpoint измерением.

## Почему это не «MGLRU backported»

- В 5.10 нет `gen_lru`; используется маленькая side table (hash по page), а не
  структуры `lru_gen` 7.4.
- Захукано несколько функций; 7.4 MGLRU охватывает `mm/vmscan.c`,
  `mm/workingset.c`, page flags, lruvec и многое другое.
- Запись `sc->priority` — грубый инструмент, а не generation-based reclaim.

## Следующие стадии

- **Fixed-endpoint измерение:** сумма `nr[]` на каждый вызов `get_scan_count`
  (трамплин `ktramp` её уже видит) и медианы по нескольким прогонам без выхода
  на OOM.
- **Stage C:** заменить выбранные `vmscan`-функции 7.4-вдохновлёнными
  реализациями через трамплин. На SMP — `aarch64_insn_patch_text` +
  `stop_machine`, не `_nosync`.
- **Stage D:** parity-subset + бенчмарки (pgscan, refault, latency, working set)
  с явным gap-list.

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
