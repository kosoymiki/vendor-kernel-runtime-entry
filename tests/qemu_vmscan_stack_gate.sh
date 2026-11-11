#!/usr/bin/env bash
# Stack gate: ktramp patches get_scan_count (policy) and vmscan_hook patches
# shrink_lruvec (custom loop). Prove both run together under real reclaim,
# then restore and unload in the safe order.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KRAMP=${KB_KRAMP:?}; KB_VHOOK=${KB_VHOOK:?}; KB_MEM=${KB_MEM:-512}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vmscan-stack.XXXXXX"); ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module,mnt}; cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod rmmod cat echo dmesg grep sleep mknod dd mkdir sync losetup sed tail; do ln -sf busybox "$ROOT/bin/$a"; done
cp -f "$KB_KRAMP" "$ROOT/module/ktramp.ko"; cp -f "$KB_VHOOK" "$ROOT/module/vmscan_hook.ko"
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
echo "=== VMSSCAN STACK ==="
insmod /module/ktramp.ko; echo "kt_ins=$?"
insmod /module/vmscan_hook.ko; echo "vh_ins=$?"
losetup /dev/loop0 /disk.img; mount -t ext4 -o ro /dev/loop0 /mnt
cat /mnt/big > /dev/null
echo 2 > /proc/ktramp; echo "kt_mode=$?"
echo 1 > /proc/vmscan_hook; echo "vh_mode=$?"
./fillmem 345 &
i=0; while [ $i -lt 90 ]; do S=$(grep '^pgscan_file ' /proc/vmstat | grep -o '[0-9]*'); [ "${S:-0}" -gt 0 ] && break; sleep 1; i=$((i+1)); done
echo "poll=$i pgscan=${S:-0}"
echo "=== KT ==="; cat /proc/ktramp
echo "=== VH ==="; cat /proc/vmscan_hook
echo 0 > /proc/vmscan_hook; echo "vh_off=$?"
echo 0 > /proc/ktramp; echo "kt_off=$?"
rmmod vmscan_hook; echo "vh_rm=$?"
rmmod ktramp; echo "kt_rm=$?"
[ -f /proc/vmscan_hook ] && echo vh_still || echo vh_gone
[ -f /proc/ktramp ] && echo kt_still || echo kt_gone
grep -q '^vmscan_hook ' /proc/modules && echo vh_mod_still || echo vh_mod_gone
grep -q '^ktramp ' /proc/modules && echo kt_mod_still || echo kt_mod_gone
echo "=== DONE ==="
while true; do sleep 5; done
INIT
chmod 755 "$ROOT/init"; ( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )
timeout 90 "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" -accel tcg,thread=single -kernel "$KB_IMAGE" -initrd "$TMP/initramfs.cpio.gz" -append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" -nographic -no-reboot > "$TMP/serial.log" 2>&1 || true
OUT=$(sed -n '/=== VMSSCAN STACK/,/=== DONE ===/p' "$TMP/serial.log" | tr -d '\r')
echo "$OUT" | grep -vE '^\s*$'
fail=0
chk() { if echo "$OUT" | grep -qE "$1"; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }
chk 'ktramp .*policy=1 calls=[1-9][0-9]* .*policy_applied=[1-9]' "ktramp patched and policy applied"
chk 'vmscan_hook .*hooked=1 calls=[1-9][0-9]* scans=[1-9]' "vmscan_hook custom loop ran"
chk 'poll=[0-9]+ pgscan=[1-9][0-9]*' "real reclaim scanned file LRU"
chk 'vh_off=0' "vmscan_hook restored"
chk 'kt_off=0' "ktramp restored"
chk 'vh_rm=0' "vmscan_hook unloaded"
chk 'kt_rm=0' "ktramp unloaded"
chk 'vh_gone' "vmscan_hook proc gone"
chk 'kt_gone' "ktramp proc gone"
chk 'vh_mod_gone' "vmscan_hook module gone"
chk 'kt_mod_gone' "ktramp module gone"
if echo "$OUT" | grep -q 'Kernel panic'; then echo "FAIL kernel panic"; fail=1; fi
[ "$fail" = 0 ] && echo "PASS vmscan stack gate" || { echo "FAIL vmscan stack gate"; exit 1; }
