#!/usr/bin/env bash
# ktramp gate: load the trampoline module, prove the wrapper is actually called
# by the reclaim path, then prove restore. QEMU single-CPU only.
#
# Why a dedicated gate (and not tests/qemu_probe_gate.sh): a trampoline is not
# proven by reading a proc file — it is proven by *call counts under real
# reclaim*. Two details make that measurable on a swapless VM:
#
#   1. tmpfs/shmem pages are NOT reclaimable without swap, and normal file
#      reads are satisfied from cache silently — neither runs the scanner.
#   2. Anonymous memory is unreclaimable, so allocating it is what pushes the
#      zone past its watermark and forces the file-LRU scan that calls
#      get_scan_count.
#
# So the workload reads block-device page cache from two brd ramdisks, then
# runs tests/fillmem.c (raw-syscall static helper) to drive kswapd/direct
# reclaim while the trampoline is installed.
#
# Env: KB_IMAGE, KB_KO, KB_MEM (default 512), BUSYBOX, QEMU, CC, QEMU_TIMEOUT
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}
KB_MEM=${KB_MEM:-512}
CC=${CC:-clang}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ktramp-gate.XXXXXX"); ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module,mnt}
cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod cat echo dmesg grep sleep poweroff mknod dd mkdir sync losetup ls; do
  ln -sf busybox "$ROOT/bin/$a"
done
cp -f "$KB_KO" "$ROOT/module/ktramp.ko"

# --- static anon-memory helper (no libc, runs on the bare Image) -----------
"$CC" -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
      -Wl,-e,_start -o "$ROOT/fillmem" "$HERE/fillmem.c"
chmod 755 "$ROOT/fillmem"

# --- evictable file workload: a real ext4 file through a loop device -------
# brd block-device reads do NOT populate the file LRU on this kernel (observed
# inactive_file:0), so the only reliable file pages come from a regular file
# mapping. A sparse zero file keeps the initramfs small after gzip.
truncate -s 40M "$TMP/disk.img"
mke2fs -t ext4 -F -q -b 4096 "$TMP/disk.img" >/dev/null 2>&1
truncate -s 32M "$TMP/bigfile"
debugfs -w -R "write $TMP/bigfile big" "$TMP/disk.img" >/dev/null 2>&1
ls -la "$TMP/disk.img"
cp -f "$TMP/disk.img" "$ROOT/disk.img"

cat > "$ROOT/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- $x; mknod /dev/$1 c $2 $3 2>/dev/null; done
mknod /dev/loop0 b 7 0
mkdir -p /mnt
echo "=== READY ==="
grep MemTotal /proc/meminfo
insmod /module/ktramp.ko; echo "insmod_rc=$?"
cat /proc/modules | grep ktramp || true
# evictable page cache: a regular file mapping on an ext4 loop device
losetup /dev/loop0 /disk.img; echo "loop_rc=$?"
mount -t ext4 -o ro /dev/loop0 /mnt; echo "mount_rc=$?"
ls -l /mnt/big
cat /mnt/big > /dev/null
cat /mnt/big > /dev/null
echo "=== BASELINE ==="
grep -E "^(MemFree|MemAvailable|Cached):" /proc/meminfo
cat /proc/ktramp
grep -E "^(pgscan_file|pgscan_direct|pgscan_kswapd)" /proc/vmstat
echo 1 > /proc/ktramp; echo "patch_rc=$?"
echo "=== PRESSURE ==="
# background: the pages must still be resident when we read the counters.
# TCG is slow, so poll until the allocation has actually eaten the free memory
# instead of guessing a sleep duration.
# 380 MiB of anon on a 470 MiB VM: free alone is not enough, the allocation has
# to exceed free+cache so the zone crosses its watermark and kswapd/direct
# reclaim scans the file LRU. Poll the actual signal (pgscan_file), not a guess.
./fillmem 345 &
i=0; S=0
while [ $i -lt 120 ]; do
  S=$(grep '^pgscan_file ' /proc/vmstat | grep -o '[0-9]*')
  [ "${S:-0}" -gt 0 ] && break
  sleep 1
  i=$((i+1))
done
echo "poll_steps=$i pgscan_file=$S free_kb=$(grep MemFree /proc/meminfo | grep -o '[0-9]*')"
echo "=== PATCHED ==="
grep -E "^(MemFree|MemAvailable|Cached):" /proc/meminfo
cat /proc/ktramp
grep -E "^(pgscan_file|pgscan_direct|pgscan_kswapd)" /proc/vmstat
echo 0 > /proc/ktramp; echo "restore_rc=$?"
echo "=== RESTORED ==="
cat /proc/ktramp
echo "=== DONE ==="
exec sleep 3600
INIT
chmod 755 "$ROOT/init"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )

"$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" -accel tcg,thread=single \
  -kernel "$KB_IMAGE" -initrd "$TMP/initramfs.cpio.gz" \
  -append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" \
  -nographic -no-reboot > "$TMP/serial.log" 2>&1 &
QPID=$!
DONE=0
for _ in $(seq 1 "${QEMU_TIMEOUT:-600}"); do
  if grep -q "=== DONE ===" "$TMP/serial.log" 2>/dev/null; then DONE=1; break; fi
  kill -0 "$QPID" 2>/dev/null || break
  sleep 1
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
RC=$([ "$DONE" = 1 ] && echo 0 || echo 1)
echo "qemu_rc=$RC log=$TMP/serial.log"
sed -n '/=== READY ===/,/=== DONE ===/p' "$TMP/serial.log" | grep -vE '^\s*$'

grep -q "insmod_rc=0" "$TMP/serial.log" || { echo "FAIL insmod"; exit 1; }
grep -q "patch_rc=0"   "$TMP/serial.log" || { echo "FAIL patch"; exit 1; }
grep -q "restore_rc=0" "$TMP/serial.log" || { echo "FAIL restore"; exit 1; }
grep -q "patched=1"    "$TMP/serial.log" || { echo "FAIL not patched"; exit 1; }
grep -q "patched=0"    "$TMP/serial.log" || { echo "FAIL restore state"; exit 1; }
CALLS=$(sed -n '/=== PATCHED ===/,/=== RESTORED ===/p' "$TMP/serial.log" | grep -o 'calls=[0-9]*' | head -1 | cut -d= -f2)
echo "calls=$CALLS"
[ -n "${CALLS:-}" ] && [ "$CALLS" -gt 0 ] || { echo "FAIL wrapper never called"; exit 1; }
if sed -n "1,/=== DONE ===/p" "$TMP/serial.log" | grep -qE "Oops|Unable to handle|Kernel panic|BUG: "; then echo "FAIL kernel fault"; exit 1; fi
echo "PASS ktramp gate (calls=$CALLS, patched, restored)"
