#!/usr/bin/env bash
# Generic QEMU probe gate: load a module, read/write a proc surface, assert.
# Env: KB_IMAGE, KB_KO, KB_MODULE_NAME, KB_PROC, KB_WRITE_VALUES (space-separated), KB_EXPECT
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}; KB_MODULE_NAME=${KB_MODULE_NAME:?}; KB_PROC=${KB_PROC:?}
KB_WRITE_VALUES=${KB_WRITE_VALUES:-}
KB_EXPECT=${KB_EXPECT:-}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kb-probe.XXXXXX"); ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module}
cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod cat echo dmesg grep sleep poweroff mknod; do ln -sf busybox "$ROOT/bin/$a"; done
cp -f "$KB_KO" "$ROOT/module/$KB_MODULE_NAME.ko"
cat > "$ROOT/init" <<INIT
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- \$x; mknod /dev/\$1 c \$2 \$3 2>/dev/null; done
echo "=== READY ==="
insmod /module/$KB_MODULE_NAME.ko; echo "insmod_rc=\$?"
cat /proc/modules | grep '$KB_MODULE_NAME' || true
echo "=== BEFORE ==="
cat /proc/$KB_PROC 2>&1 || true
for v in $KB_WRITE_VALUES; do echo "\$v" > /proc/$KB_PROC; echo "write_\$v_rc=\$?"; done
echo "=== AFTER ==="
cat /proc/$KB_PROC 2>&1 || true
echo "=== DONE ==="
poweroff -f
INIT
chmod 755 "$ROOT/init"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )
set +e
timeout "${QEMU_TIMEOUT:-180}" "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m 512 -accel tcg,thread=single \
  -kernel "$KB_IMAGE" -initrd "$TMP/initramfs.cpio.gz" \
  -append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" -nographic -no-reboot > "$TMP/serial.log" 2>&1
RC=$?
set -e
echo "qemu_rc=$RC log=$TMP/serial.log"
grep -E "insmod_rc=|write_|BEFORE|AFTER|patched=|policy_mode=|mglru_probe|FAIL|PASS" "$TMP/serial.log" | tail -30
grep -q "insmod_rc=0" "$TMP/serial.log" || { echo FAIL insmod; exit 1; }
if [ -n "$KB_EXPECT" ]; then grep -q "$KB_EXPECT" "$TMP/serial.log" || { echo "FAIL expect $KB_EXPECT"; exit 1; }; fi
echo "PASS qemu probe gate"
