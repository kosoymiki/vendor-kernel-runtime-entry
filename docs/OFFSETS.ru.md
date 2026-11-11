# Смещения ядра, используемые runtime-модулями (проверены на цели)

Каждое смещение ниже наблюдалось на работающем vendor `Image` (5.10.236)
QEMU-пробой, а не скопировано из другого ядра. Проба (`scprobe`) ставит kprobe
на `shrink_node` и печатает байты `struct scan_control` на входе.

## `struct scan_control` (приватна для `mm/vmscan.c`)

| поле | смещение | наблюдалось |
|---|---:|---|
| `nr_to_reclaim` | `0x00` | 2047 (kswapd) / 32 (direct) |
| битфилды (`may_deactivate:2`, `force_deactivate:1`, `skipped_deactivate:1`, …) | `0x28` | 0 |
| `order` | `0x2a` | 0 |
| **`priority`** | **`0x2b`** | **12 → 11 → 10 → 9** (DEF_PRIORITY убывает) |
| `reclaim_idx` | `0x2c` | 0 / 2 (индекс зоны) |
| `gfp_mask` | `0x30` | `0xcc0` / `0x3100dca` |
| `nr_scanned` | `0x38` | 0 на входе `shrink_node` |
| `nr_reclaimed` | `0x40` | 0 → 11 во время reclaim |

**Исправление:** ранняя ревизия `mglru_probe` использовала `0x2c` для
`priority`. Проба показывает: `0x2c` — это `reclaim_idx`, а `priority` —
`0x2b`. `mglru_probe` 1.0.6 и `vmscan_hook` используют `0x2b`. Это реальный
дефект: «priority policy» писала индекс зоны reclaim вместо приоритета.

Почему layout необычный: 13 битфилдов упакованы clang в два байта, поэтому
`order/priority/reclaim_idx` начинаются с `0x2a`, а не с `0x2c`.

## `struct lruvec`

| поле | смещение | источник |
|---|---:|---|
| `nonresident_age` | `0x60` | `include/linux/mmzone.h:290` (lists 0x00–0x4f, anon_cost 0x50, file_cost 0x58) |
| `refaults[FILE]` | `0x70` | `mmzone.h:292` |
| `flags` | `0x78` | `mmzone.h:294` |
| `pgdat` | `0x80` | `mmzone.h:296` |

(Наблюдено readback модуля: `lruvec.size=136`, `flags=120`, `pgdat=128`.)

## Workingset shadow

`workingset_eviction` пакует `nonresident_age` в shadow; на этом ядре
`eviction = shadow >> 19` (`xa_to_value >>2`, бит workingset `>>1`,
`NODES_SHIFT=0`, `MEM_CGROUP_ID_SHIFT=16`, `bucket_order=0`). Refault distance =
`(nonresident_age_now - eviction) & ((1<<46)-1)`.

## Как перепроверить

`modules/scprobe` — диагностический модуль (не входит в релизные артефакты):
загрузить, запустить reclaim, прочитать `dmesg | grep scprobe`. Он печатает
`o2a p2b r2c x2d scan38 recl40 to0 gfp30`. Изменение любого значения
инвалидирует смещения выше, и модули нужно пересобрать под новую цель.
