# MGLRU probe: статус Stage A/B (экспериментально)

`modules/mglru` — **probe**, не полный backport 7.4 MGLRU. Он измеряет reclaim
path 5.10 и проверяет generation-aware политики через доказанный entry.

## Что делает

- **Stage A (read-only):** kprobes на `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`; счётчики в `/proc/mglru_probe`.
- **Stage B (policy):** kretprobe на `get_scan_count`; режимы:
  `1` anon-light, `2` file-light, `3` aggressive, `4` generation-aware.
- `mark_page_accessed` kprobe заполняет side table поколений (1024 записи).
- Mode 4 считает `tracked/young/ratio` и срезает scans, если молодых много,
  иначе удваивает file scans.

## Наблюдения в QEMU (exact Image)

- Stage A: reclaim hooks 426 hits; `page_referenced` 0 в этой нагрузке.
- Mode 2: `modified=796`, `changed=305`, `before=0,0,1,1` → `after=0,0,0,0`.
- Mode 4: `acc=10786`, `trk=250`, `yng=250`, `ratio=100`,
  `changed=368`, `before=0,0,2,3` → `after=0,0,1,1`.
- `gen` остался 1 в captured run при большом `modified`; семантика advance
  требует проверки. Не использовать это как parity-evidence.

Логи: `$PREFIX/tmp/mglru-*` (QEMU). Это результаты probe, не бенчмарк.

## Как правильно измерять reclaim

Ранние прогоны давали `pgscan_file=0` и `changed=0`, потому что нагрузка не
доводила до сканера: страницы `tmpfs` не реклеймятся без swap, а обычные
аллокации молча удовлетворяются из page cache. Рабочая методика — ext4-файл
через `loop0` плюс анонимное давление с поллингом `pgscan_file` — описана в
[`docs/MEASURING-RECLAIM.md`](MEASURING-RECLAIM.md). С ней трамплин-проба
видит `calls=113` при `pgscan_file=6084`.

Любая следующая стадия MGLRU обязана приводить дельты `pgscan_*` с этой
нагрузки; иначе результат не измерен.

## Почему это не «MGLRU backported»

- В 5.10 нет `gen_lru`; используется маленькая side table (hash по page), а не
  структуры `lru_gen` 7.4.
- Захукано лишь несколько функций; 7.4 MGLRU охватывает `mm/vmscan.c`,
  `mm/workingset.c`, page flags, lruvec и многое другое.
- Политика эвристическая; нет parity-метрик/доказательства качества reclaim.

## Следующие стадии

- **Stage C:** заменить выбранные `vmscan`-функции 7.4-вдохновлёнными
  реализациями через text patch/trampoline. На SMP — `aarch64_insn_patch_text`
  + stop_machine (не `nosync`), W^X/CFI-safe trampolines, rollback.
- **Stage D:** parity subset + benchmark (pgscan, refault, latency, working
  set); документировать gaps.

## Сборка

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
```

Эксперимент: QEMU first; disable policy (`write 0`) before unload; no `rmmod`
на устройстве до доказанного unload-path.
