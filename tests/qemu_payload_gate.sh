#!/usr/bin/env bash
# QEMU gate: boot the exact stock Image, insmod the module, assert
# insmod_rc=0 and payload_result=42. No device writes.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?set KB_IMAGE}
KB_KO=${KB_KO:?set KB_KO}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}
QEMU=${QEMU:-qemu-system-aarch64}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kb-qemu.XXXXXX")
R=$TMP/root
mkdir -p "$R"/{bin,proc,sys,dev,tmp,module}
cp -f "$BUSYBOX" "$R/bin/busybox"; chmod 755 "$R/bin/busybox"
for a in sh mount insmod cat echo dmesg grep sleep poweroff mknod; do ln -sf busybox "$R/bin/$a"; done
cp -f "$KB_KO" "$R/module/kbootstrap.ko"
cat > "$R/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- $x; mknod /dev/$1 c $2 $3 2>/dev/null; done
echo "=== READY ==="
insmod /module/kbootstrap.ko
echo "insmod_rc=$?"
cat /proc/modules | grep kbootstrap || true
echo "=== PROC ==="
cat /proc/kbootstrap 2>&1 || true
echo "=== DONE ==="
poweroff -f
INIT
chmod 755 "$R/init"
( cd "$R" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )
set +e
timeout "${QEMU_TIMEOUT:-180}" "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m 512 \
  -accel tcg,thread=single -kernel "$KB_IMAGE" -initrd "$TMP/initramfs.cpio.gz" \
  -append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" \
  -nographic -no-reboot > "$TMP/serial.log" 2>&1
RC=$?
set -e
echo "qemu_rc=$RC log=$TMP/serial.log"
grep -E "insmod_rc=|payload_result=|kbootstrap-poc|kbootstrap: ready|=== DONE ===" "$TMP/serial.log" || true
grep -q "insmod_rc=0" "$TMP/serial.log" || { echo "FAIL insmod"; exit 1; }
grep -q "payload_result=42" "$TMP/serial.log" || { echo "FAIL payload_result"; exit 1; }
echo "PASS qemu payload gate"
