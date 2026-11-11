# scprobe — scan_control offset probe (diagnostic)

> Part of a library under **active development**. This is a diagnostic module,
> not a release artifact.

Kprobes `shrink_node` and prints `struct scan_control` bytes at entry, so the
offsets used by `ktramp`/`mglru_probe`/`vmscan_hook` are observed on the target
instead of assumed.

```
insmod scprobe.ko
# run reclaim, then:
dmesg | grep scprobe
# scprobe: sc=... o2a=0 p2b=12 r2c=0 x2d=0 scan38=0 recl40=0 to0=2047 gfp30=3264
```

`p2b` is `priority` (12→11→10→9); `r2c` is `reclaim_idx`. See
[`docs/OFFSETS.md`](../../docs/OFFSETS.md).
