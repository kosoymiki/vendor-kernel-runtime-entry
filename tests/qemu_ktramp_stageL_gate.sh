#!/usr/bin/env bash
# ktramp Stage L/M gate: hot-page bloom filter, tiered generation scanning and
# the MGLRU min_ttl floor, proven on the real reclaim path.
#
# One boot, three phases:
#   A  mode 3 under pressure: the bloom filter must be populated and rotating,
#      and the tiered decision must fire (branch 10 protect-hot or 11
#      scan-oldest) because the oldest resident generation is non-empty.
#   B  min_ttl raised to 60 s: every epoch is younger than the floor, so the
#      deferral branch 12 must grow and ttlfloor must increase.
#   C  restore + unload: patch off, rmmod, /proc and module gone.
#
# Env: KB_IMAGE, KB_KO (required), KB_MEM (512), BUSYBOX, QEMU, CC.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}
KB_MEM=${KB_MEM:-512}
CC=${CC:-clang}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ktramp-stagel.XXXXXX")

truncate -s 40M "$TMP/disk.img"
mke2fs -t ext4 -F -q -b 4096 "$TMP/disk.img" >/dev/null 2>&1
truncate -s 32M "$TMP/bigfile"
debugfs -w -R "write $TMP/bigfile big" "$TMP/disk.img" >/dev/null 2>&1

"$CC" -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
      -Wl,-e,_start -o "$TMP/fillmem" "$HERE/fillmem.c"
chmod 755 "$TMP/fillmem"

ROOT=$TMP/root
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp,module,mnt}
cp -f "$BUSYBOX" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox"
for a in sh mount insmod rmmod cat echo dmesg grep sleep mknod dd mkdir sync losetup sed tail; do
	ln -sf busybox "$ROOT/bin/$a"
done
cp -f "$KB_KO" "$ROOT/module/ktramp.ko"
cp -f "$TMP/disk.img" "$ROOT/disk.img"
cp -f "$TMP/fillmem" "$ROOT/fillmem"; chmod 755 "$ROOT/fillmem"
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- $x; mknod /dev/$1 c $2 $3 2>/dev/null; done
mknod /dev/loop0 b 7 0 2>/dev/null
echo "=== KTRAMP stagel ==="
insmod /module/ktramp.ko; echo "insmod_rc=$?"
losetup /dev/loop0 /disk.img; echo "loop_rc=$?"
mount -t ext4 -o ro /dev/loop0 /mnt; echo "mount_rc=$?"
cat /mnt/big > /dev/null
echo 3 > /proc/ktramp; echo "mode_rc=$?"
echo m0 > /proc/ktramp; echo "minttl0_rc=$?"
./fillmem 345 &
i=0
while [ $i -lt 200 ]; do
  C=$(cat /proc/ktramp | sed -n 's/.*nr_calls=\([0-9]*\).*/\1/p')
  [ "${C:-0}" -ge 20 ] && break
  sleep 1; i=$((i+1))
done
echo "poll=$i nr_calls=${C:-0}"
echo "=== A ==="
cat /proc/ktramp | sed 's/^/A /'
echo "=== B ==="
echo m60000 > /proc/ktramp; echo "minttl_rc=$?"
./fillmem 120 &
sleep 25
cat /proc/ktramp | sed 's/^/B /'
echo "=== C ==="
echo 0 > /proc/ktramp; echo "restore_rc=$?"
cat /proc/ktramp | sed 's/^/C /' 
rmmod ktramp; echo "rmmod_rc=$?"
[ -f /proc/ktramp ] && echo proc_still_present || echo proc_gone
grep -q '^ktramp ' /proc/modules && echo module_still_loaded || echo module_gone
echo "=== DONE ==="
while true; do sleep 5; done
INIT
chmod 755 "$ROOT/init"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs.cpio.gz" )
timeout 300 "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" \
	-accel tcg,thread=single -kernel "$KB_IMAGE" \
	-initrd "$TMP/initramfs.cpio.gz" \
	-append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" \
	-nographic -no-reboot > "$TMP/serial.log" 2>&1 || true

# Serial console output can interleave with the marker echoes, so the counters
# are tagged in the guest (A/B/C prefix) instead of parsed by line ranges.
tr -d '\r' < "$TMP/serial.log" > "$TMP/serial.txt"
A=$(grep '^A ktramp ' "$TMP/serial.txt" | tail -1)
B=$(grep '^B ktramp ' "$TMP/serial.txt" | tail -1)
C=$(grep '^C ktramp ' "$TMP/serial.txt" | tail -1)
field() { # section-value field
	echo "$1" | sed -n 's/.*'"$2"'=\([^ ]*\).*/\1/p' | head -1
}
branch_at() { # section-value index
	echo "$1" | sed -n 's/.* branch=\([0-9,]*\).*/\1/p' | cut -d, -f"$2"
}
bloom_at() { # section-value index
	echo "$1" | sed -n 's/.* bloom=\([0-9,]*\).*/\1/p' | cut -d, -f"$2"
}
fail=0
check() { if [ "$1" = 1 ]; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi }

echo "COUNTERS A: $A"
echo "COUNTERS B: $B"
echo "COUNTERS C: $C"

a_pol=$(field "$A" policy); a_calls=$(field "$A" nr_calls)
a_set=$(bloom_at "$A" 1); a_rot=$(bloom_at "$A" 4)
a_b10=$(branch_at "$A" 10); a_b11=$(branch_at "$A" 11)
a_floor=$(field "$A" ttlfloor)

check "$([ "$a_pol" = 1 ] && [ "${a_calls:-0}" -gt 0 ] && echo 1 || echo 0)" "A: mode3 policy=1 nr_calls=$a_calls"
check "$([ "${a_set:-0}" -gt 0 ] && echo 1 || echo 0)" "A: bloom bits set ($a_set)"
check "$([ "${a_rot:-0}" -gt 0 ] && echo 1 || echo 0)" "A: bloom rotated with epochs ($a_rot)"
check "$([ $(( ${a_b10:-0} + ${a_b11:-0} )) -gt 0 ] && echo 1 || echo 0)" "A: tiered decision fired (b10=$a_b10 b11=$a_b11)"

b_floor=$(field "$B" ttlfloor)
b_b12=$(branch_at "$B" 12)
b_minttl=$(field "$B" minttl)
check "$([ "${b_minttl:-0}" = 60000 ] && echo 1 || echo 0)" "B: min_ttl floor set to ${b_minttl}ms"
check "$([ "${b_b12:-0}" -gt 0 ] && echo 1 || echo 0)" "B: deferral branch 12 fired ($b_b12)"
check "$([ "${b_floor:-0}" -gt "${a_floor:-0}" ] && echo 1 || echo 0)" "B: ttlfloor grew ($a_floor -> $b_floor)"

c_patched=$(field "$C" patched)
c_restore=$(grep -c '^restore_rc=0$' "$TMP/serial.txt" || true)
c_rmmod=$(grep -c '^rmmod_rc=0$' "$TMP/serial.txt" || true)
c_proc=$(grep -c '^proc_gone$' "$TMP/serial.txt" || true)
c_mod=$(grep -c '^module_gone$' "$TMP/serial.txt" || true)
check "$([ "$c_patched" = 0 ] && [ "$c_restore" = 1 ] && [ "$c_rmmod" = 1 ] && [ "$c_proc" = 1 ] && [ "$c_mod" = 1 ] && echo 1 || echo 0)" \
      "C: restore+unload patched=$c_patched restore=$c_restore rmmod=$c_rmmod proc_gone=$c_proc module_gone=$c_mod"

echo "log: $TMP/serial.log"
[ "$fail" = 0 ] && echo "PASS ktramp Stage L/M gate" || { echo "FAIL ktramp Stage L/M gate"; exit 1; }
