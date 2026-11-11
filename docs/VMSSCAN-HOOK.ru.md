# vmscan_hook — кастомный `shrink_lruvec`, внедрённый модулем

Это точка входа для кастомного reclaim path без пересборки vmscan: модуль
заменяет `shrink_lruvec` своей функцией, а она вызывает штатные
`shrink_active_list` / `shrink_inactive_list` для выбранных страниц.

## Почему это возможно на 5.10.236

- `CONFIG_KALLSYMS_ALL=y`: `shrink_lruvec`, `get_scan_count`,
  `shrink_active_list`, `shrink_inactive_list` — локальные символы, но
  резолвятся через `kallsyms_lookup_name` (адрес которого берётся временным
  kprobe).
- `CONFIG_STRICT_KERNEL_RWX=y`: `aarch64_insn_patch_text` делает
  stop_machine-safe обновление текста (тот же механизм, что в `kpatch`/`ktramp`).
- Vendor CFI runtime удовлетворяется CFI-шимом модуля (`__cfi_check`,
  `__cfi_jt_*`), `MODVERSIONS` — сгенерированными `__versions`, а layout
  `this_module` — шагом postprocess.
- ftrace/livepatch не нужны (и не сконфигурированы).

## Механизм

```
вход shrink_lruvec (патч: b my_shrink_lruvec)
        |
        v
my_shrink_lruvec(lruvec, sc)              [модуль]
   get_scan_count(lruvec, sc, nr)         [ядро, через kallsyms]
   while nr[]:
       for lru in INACTIVE_ANON..ACTIVE_FILE:
           shrink_active_list(...)        [ядро, через kallsyms]
           shrink_inactive_list(...)      [ядро, через kallsyms]
       стоп, когда sc->nr_reclaimed - r0 >= sc->nr_to_reclaim
```

Смещения (проверены `scprobe`, см. [`OFFSETS.ru.md`](OFFSETS.ru.md)):
`nr_to_reclaim 0x00`, bits `0x28`, `gfp_mask 0x30`, `nr_scanned 0x38`,
`nr_reclaimed 0x40`.

`/proc/vmscan_hook`: запись `1` — установить, `0` — восстановить; чтение —
`hooked`, `calls`, `scans`, `reclaimed`.

## Наблюдения в QEMU (stock `Image`, 512 MiB, без swap)

`tests/qemu_vmscan_hook_gate.sh`, модуль sha `3fb1610c…`:

```
insmod_rc=0
hook_rc=0
poll=10 pgscan=8196
vmscan_hook target=... hooked=1 calls=34 scans=8209 reclaimed=0
restore_rc=0
vmscan_hook target=... hooked=0 calls=34 scans=8209 reclaimed=0
rmmod_rc=0
proc_gone
module_gone
```

Кастомная функция отработала 34 раза под реальным reclaim, просканировала
8209 страниц штатными сканерами списков, reclaim path остался жив
(`pgscan_file=8196`). Restore и unload чистые; паники нет.

## Ограничения (честно)

- Цикл намеренно упрощён: нет пропорциональной балансировки anon/file,
  `blk_start_plug` и эвристики `scan_adjusted`. `reclaimed` в readback — дельта
  последнего вызова.
- `get_scan_count` вызывается для целей сканирования; полностью кастомная
  поколенческая политика может заменить этот вызов вычислением в модуле.
- Заменяется только `shrink_lruvec`. `shrink_node_memcgs` по-прежнему
  перебирает memcg; кастомный `shrink_node` — отдельная цель патча.
- Патч текста — привилегированная операция на конкретном ядре: модуль
  отказывается патчить при выходе ветвления за диапазон, а `write 0`
  восстанавливает оригинальную инструкцию. Сначала QEMU; reboot — предельный
  rollback.

## Сборка и гейт

```sh
KB_MODULE_SRC=$PWD/modules/vmscan_hook KB_MODNAME=vmscan_hook \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/vmscan_hook/vmscan_hook.ko \
  bash tests/qemu_vmscan_hook_gate.sh
```
