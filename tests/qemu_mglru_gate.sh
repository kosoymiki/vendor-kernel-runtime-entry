#!/usr/bin/env bash
# MGLRU probe gate: run the reclaim-path probes against a workload that really
# scans the file LRU, in a chosen policy mode, and print the measured counters.
#
# This is the A/B harness behind docs/MGLRU.md: run MODE=0 for the baseline and
# MODE=5 for the aggressive shrink_node priority policy, then compare
# get_scan_count hits and pgscan_file.
#
# Why the workload looks like this (docs/MEASURING-RECLAIM.md): tmpfs never
# scans without swap, brd reads do not populate the file LRU here, and normal
# allocations are served from cache silently. Evictable file pages come from a
# real ext4 file through loop0; unreclaimable anonymous memory (tests/fillmem.c)
# is what pushes the zone past its watermark.
#
# Env: MODE (0..5), KB_IMAGE, KB_KO, KB_MEM (default 512), BUSYBOX, QEMU, TMPDIR
set -euo pipefail
MODE=${MODE:-0}
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}
KB_MEM=${KB_MEM:-512}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)

TMP=$(mktemp -d "${TMPDIR:-/tmp}/mglru-gate.XXXXXX"); ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module,mnt}
cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod cat echo dmesg grep sleep mknod dd mkdir sync losetup; do
  ln -sf busybox "$ROOT/bin/$a"
done
cp -f "$KB_KO" "$ROOT/module/mglru_probe.ko"

# real file on ext4 -> evictable file-LRU pages
truncate -s 40M "$TMP/disk.img"
mke2fs -t ext4 -F -q -b 4096 "$TMP/disk.img" >/dev/null 2>&1
truncate -s 32M "$TMP/bigfile"
debugfs -w -R "write $TMP/bigfile big" "$TMP/disk.img" >/dev/null 2>&1
cp -f "$TMP/disk.img" "$ROOT/disk.img"

CC=${CC:-clang}
"$CC" -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
      -Wl,-e,_start -o "$ROOT/fillmem" "$HERE/fillmem.c"
chmod 755 "$ROOT/fillmem"

cat > "$ROOT/init" <<INIT
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- \$x; mknod /dev/\$1 c \$2 \$3 2>/dev/null; done
mknod /dev/loop0 b 7 0
echo "=== MGLRU mode=$MODE ==="
insmod /module/mglru_probe.ko; echo "insmod_rc=\$?"
losetup /dev/loop0 /disk.img; mount -t ext4 -o ro /dev/loop0 /mnt; echo "mount_rc=\$?"
cat /mnt/big > /dev/null
echo "=== BASELINE ==="
cat /proc/mglru_probe
grep -E "^(pgscan_file|pgscan_direct|pgscan_kswapd|pgsteal_file)" /proc/vmstat
echo $MODE > /proc/mglru_probe; echo "mode_rc=\$?"
./fillmem 345 &
i=0
while [ \$i -lt 120 ]; do
  S=\$(grep '^pgscan_file ' /proc/vmstat | grep -o '[0-9]*')
  [ "\${S:-0}" -gt 0 ] && break
  sleep 1
  i=\$((i+1))
done
echo "poll_steps=\$i pgscan_seen=\${S:-0}"
echo "=== PATCHED ==="
cat /proc/mglru_probe
grep -E "^(pgscan_file|pgscan_direct|pgscan_kswapd|pgsteal_file)" /proc/vmstat
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
for _ in $(seq 1 "${QEMU_TIMEOUT:-420}"); do
  if grep -q "=== DONE ===" "$TMP/serial.log" 2>/dev/null; then DONE=1; break; fi
  kill -0 "$QPID" 2>/dev/null || break
  sleep 1
done
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo "qemu_done=$DONE log=$TMP/serial.log"
sed -n '/=== MGLRU mode=/,/=== DONE ===/p' "$TMP/serial.log" | grep -vE '^\s*$'

grep -q "insmod_rc=0" "$TMP/serial.log" || { echo "FAIL insmod"; exit 1; }
grep -q "mode_rc=0"   "$TMP/serial.log" || { echo "FAIL mode write"; exit 1; }
BASE=$(sed -n '/=== BASELINE ===/,/=== PATCHED ===/p' "$TMP/serial.log" | grep -o 'pgscan_file [0-9]*' | head -1 | awk '{print $2}')
AFTER=$(sed -n '/=== PATCHED ===/,/=== DONE ===/p' "$TMP/serial.log" | grep -o 'pgscan_file [0-9]*' | head -1 | awk '{print $2}')
HITS=$(sed -n '/=== PATCHED ===/,/=== DONE ===/p' "$TMP/serial.log" | grep -o 'get_scan_count: reg=1 err=0 hits=[0-9]*' | grep -o '[0-9]*$' | head -1)
echo "summary mode=$MODE pgscan_before=${BASE:-0} pgscan_after=${AFTER:-0} get_scan_count_hits=${HITS:-0}"
if grep -qE "Oops|Unable to handle|Kernel panic|BUG: " "$TMP/serial.log"; then echo "FAIL kernel fault"; exit 1; fi
grep -q "=== DONE ===" "$TMP/serial.log" || { echo "FAIL workload never completed"; exit 1; }
echo "PASS mglru gate (mode=$MODE)"
