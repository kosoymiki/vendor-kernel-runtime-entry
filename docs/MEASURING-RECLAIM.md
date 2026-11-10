# Measuring reclaim in QEMU (and why naive workloads measure nothing)

Any MGLRU/policy work needs one thing before it needs code: **proof that the
kernel actually scanned the file LRU**. Without it, `changed=0` and
`pgscan_file=0` look like a working policy. This document records the
measurement methodology that produced the numbers quoted in the READMEs.

Reference setup: QEMU `virt`, `-cpu cortex-a72 -smp 1 -m 512`, the exact stock
vendor `Image`, `rdinit=/init`, no swap.

## The four traps

### 1. `tmpfs` never triggers the scanner

`shmem` pages are only reclaimable when there is swap. On a swapless VM, writing
hundreds of megabytes into `tmpfs` produces this:

```
MemFree:           31024 kB
Cached:           411292 kB      <- almost all of it shmem
pgscan_file          0
pgscan_kswapd        0
```

No scan, no `get_scan_count`, and `tmpfs` hits its 50%-of-RAM size limit first —
which is where the `ENOSPC` in earlier experiments came from. **Do not use tmpfs
as pressure.**

### 2. A char-node "ramdisk" is `/dev/null`

```sh
mknod /dev/ram0 c 1 0     # WRONG: char 1,0 is /dev/null
mknod /dev/ram0 b 1 0     # correct block node
```

With the char node, `dd if=/dev/ram0` reads `/dev/null`, `Cached` stays at
~1.7 MB and the experiment silently measures nothing.

### 3. `brd` block-device reads do not populate the file LRU here

Reading `/dev/ram0` (a real block node) still leaves the LRU empty on this
kernel:

```
Node 0 active_file:0kB inactive_file:0kB ...
12713 total pagecache pages        <- shmem only
```

So a ramdisk is not a substitute for a file-backed workload on 5.10.236.

### 4. Normal allocations are satisfied from cache silently

Free memory can fall to a few tens of megabytes without a single scan, because
page-cache pages are handed to allocations directly. The scanner only runs when
a zone crosses its watermark.

## The workload that works

**Evictable file pages from a real file mapping + unreclaimable anonymous
pressure.**

1. **A real file.** Build an ext4 image on the host, put a 32 MiB file in it,
   mount it read-only through the loop driver inside the VM:

   ```sh
   truncate -s 40M disk.img && mke2fs -t ext4 -F -q -b 4096 disk.img
   truncate -s 32M bigfile
   debugfs -w -R "write $PWD/bigfile big" disk.img
   # inside the VM:
   mknod /dev/loop0 b 7 0
   losetup /dev/loop0 /disk.img && mount -t ext4 -o ro /dev/loop0 /mnt
   cat /mnt/big > /dev/null
   ```

   `cat` creates page-cache pages on the **file LRU** for both the ext4 file
   mapping and the loop device. These are clean and evictable.

2. **Anonymous pressure.** Allocate and touch unreclaimable memory until the zone
   crosses its watermark. `tests/fillmem.c` is a 2 KB freestanding ELF that does
   exactly this with raw syscalls (`mmap` 222, `nanosleep` 101), because Termux
   clang cannot link `-static` against libc:

   ```sh
   clang -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
         -Wl,-e,_start -o fillmem tests/fillmem.c
   ```

   Two details matter and both were bugs first:

   - the entry point must be `naked` — a normal C prologue moves `sp` before
     `argv` is read, so `fillmem 345` silently allocates the default 128 MiB;
   - under TCG, touching hundreds of MiB takes tens of seconds, so **poll**,
     never `sleep`.

3. **Poll the actual signal.**

   ```sh
   ./fillmem 345 &
   while :; do
     S=$(grep '^pgscan_file ' /proc/vmstat | grep -o '[0-9]*')
     [ "${S:-0}" -gt 0 ] && break
     sleep 1
   done
   ```

   `pgscan_file` is the ground truth: pages actually scanned on the file LRU.

## Reference run

QEMU, `-m 512`, 32 MiB ext4 file, 345 MiB anonymous pressure:

```
=== BASELINE ===
MemFree:          334516 kB
Cached:           108192 kB
pgscan_file            0
=== PRESSURE ===
fillmem: touched
poll_steps=10 pgscan_file=6084 free_kb=4276
=== PATCHED ===
ktramp ... patched=1 calls=113
pgscan_kswapd       5917
pgscan_direct        167
pgscan_file         6084
=== RESTORED ===
ktramp ... patched=0 calls=113
```

`calls=113` is the trampoline wrapper being entered by the real reclaim path —
this is what makes `ktramp` a proof and not a demo.

## Budgets

| knob | value | why |
|---|---|---|
| VM RAM | 512 MiB | matches the reference device class, keeps TCG fast |
| file | 32 MiB | enough evictable pages to scan repeatedly |
| anonymous | 345 MiB | crosses the watermark, leaves ~100 MiB of cache to reclaim |
| poll limit | 120 s | TCG is slow; the touch phase is the bottleneck |

Do **not** push anonymous memory past free + cache: the OOM killer ends the
process (and on this kernel it can deadlock the box) before any scan happens.
If you see `Out of memory: Killed process ... fillmem`, lower the size.

## Where this is implemented

- [`tests/fillmem.c`](../tests/fillmem.c) — the freestanding pressure helper.
- [`tests/qemu_ktramp_gate.sh`](../tests/qemu_ktramp_gate.sh) — full gate:
  build helper, ext4 image, loop mount, patch, pressure, assert
  `calls > 0` + `patched=1`, restore, fault scan.
