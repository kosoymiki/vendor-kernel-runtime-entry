#!/usr/bin/env bash
# vmscan_hook gate: replace shrink_lruvec with a module implementation, prove
# it runs under real reclaim, then restore and unload.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}; KB_MEM=${KB_MEM:-512}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vmscan-hook.XXXXXX"); ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module,mnt}; cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod rmmod cat echo dmesg grep sleep mknod dd mkdir sync losetup sed tail; do ln -sf busybox "$ROOT/bin/$a"; done
cp -f "$KB_KO" "$ROOT/module/vmscan_hook.ko"
clang -O2 -static -nostdlib -fno-stack-protector -ffreestanding -Wl,-e,_start -o "$ROOT/fillmem" "$HERE/fillmem.c"
truncate -s 40M "$TMP/disk.img"; mke2fs -t ext4 -F -q -b 4096 "$TMP/disk.img" >/dev/null 2>&1
truncate -s 32M "$TMP/big"; debugfs -w -R "write $TMP/big big" "$TMP/disk.img" >/dev/null 2>&1
cp -f "$TMP/disk.img" "$ROOT/disk.img"
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- $x; mknod /dev/$1 c $2 $3 2>/dev/null; done
mknod /dev/loop0 b 7 0 2>/dev/null
echo "=== VMSSCAN HOOK ==="
insmod /module/vmscan_hook.ko; echo "insmod_rc=$?"
losetup /dev/loop0 /disk.img; mount -t ext4 -o ro /dev/loop0 /mnt
cat /mnt/big > /dev/null
while true; do cat /mnt/big > /dev/null; done &
echo t20000 > /proc/vmscan_hook
echo 1 > /proc/vmscan_hook; echo "hook_rc=$?"
./fillmem 345 &
i=0; while [ $i -lt 120 ]; do S=$(grep '^pgscan_file ' /proc/vmstat | grep -o '[0-9]*'); [ "${S:-0}" -gt 0 ] && break; sleep 1; i=$((i+1)); done
echo "poll=$i pgscan=${S:-0}"
echo "=== COUNTERS ==="; cat /proc/vmscan_hook
grep -qE "moved=[1-9]" /proc/vmscan_hook && echo "PASS moved>0" || echo "FAIL moved=0"
echo 0 > /proc/vmscan_hook; echo "restore_rc=$?"; cat /proc/vmscan_hook
rmmod vmscan_hook; echo "rmmod_rc=$?"
[ -f /proc/vmscan_hook ] && echo proc_still_present || echo proc_gone
grep -q '^vmscan_hook ' /proc/modules && echo module_still_loaded || echo module_gone
echo "=== DONE ==="
while true; do sleep 5; done
INIT
chmod 755 "$ROOT/init"; ( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )
timeout 150 "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" -accel tcg,thread=single -kernel "$KB_IMAGE" -initrd "$TMP/initramfs.cpio.gz" -append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" -nographic -no-reboot > "$TMP/serial.log" 2>&1 || true
echo log=$TMP/serial.log; sed -n '/=== VMSSCAN HOOK/,/=== DONE ===/p' "$TMP/serial.log" | tr -d '\r' | grep -vE '^\s*$'
