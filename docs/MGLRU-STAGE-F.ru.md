# MGLRU Stage F — M4 parity-subset: per-memcg часы scan-эпох

Stage F заменяет глобальный счётчик поколений (64 скана) на **per-memcg часы
scan-эпох** из vendor-tracepoint reclaim и масштабирует порог refault окном
поколений. Это runtime parity-subset для M4; пересборка vmscan не нужна.

## Механизм

- Vendor-tracepoint `android_vh_shrink_node_memcgs` срабатывает на каждый скан
  memcg. `tp_memcg_probe` увеличивает `seq` этой memcg и ставит
  `window = min(seq, 2)` — аналог `max_seq - min_seq`.
- `mark_page_accessed` сохраняет `seq` страницы в folio shadow table.
- `workingset_eviction` сравнивает поколение страницы с текущим `seq` memcg:
  равенство — **young** (доступ в текущей эпохе), иначе **cold**.
- Политика `get_scan_count`:
  1. refault distance `d`: `d <= 32 * window` обнуляет file-сканы, иначе
     делит пополам;
  2. young > 60%: вдвое уменьшить file-сканы;
  3. young < 20%: оставить полный скан;
  4. иначе — чётность поколения per-memcg;
  5. `can_swap` последним, только обнуляет anon-счётчики.
- Readback добавляет `seq=` и `win=`; политика по-прежнему инъектируема
  (`t5` distance, `y80` young/cold).

## Наблюдения в QEMU (stock `Image`, 512 MiB, без swap)

`tests/qemu_ktramp_gate.sh`, модуль sha `34bf93d0…`:

| режим | счётчики | ветка | результат |
|---|---|---|---|
| холодный ext4-кэш | `seq=125`, `win=2`, `nr_calls=125` | 7 (полный скан) | `delta20=2 / pre20=608` |
| форс young `y80` | `seq=133`, `win=2` | 6 | `delta20=591 / pre20=1167` = **50.64%** |
| форс distance `t5` | `seq=25`, `win=2` | 1 | `delta20=8703 / pre20=8703` = **100%** |
| реальный refault | `seq=141`, `win=2` | 2 | `delta20=376 / pre20=735`, `tdist=847` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

Часы двигаются реальным reclaim (`seq` 25 → 141 по режимам), окно
насыщается на 2 — соответствует задуманному двухпоколенческому окну.

## Тесты

`modules/ktramp/tests/test_policy.c`: 31/31 PASS, включая порог по окну
(`d=40 <= 64` обнуляет file, `d=70 > 64` делит пополам).

## Ограничения

- Эпоха двигается на каждый скан memcg (vendor-tracepoint), а не в точной
  точке MGLRU, где создаётся новое поколение; per-generation LRU-списки —
  по-прежнему side table, а не структуры `lru_gen`.
- Окно фиксировано в две эпохи; адаптивный `min_ttl` — работа M5.
- Политика меняет только `nr[]`; структуры ядра не модифицируются.
