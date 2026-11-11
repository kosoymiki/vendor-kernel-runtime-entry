#!/usr/bin/env bash
# ktramp Stage E gate: prove the generation-tier policy on the real reclaim
# path, one QEMU boot per regime, then restore + unload.
#
# Regimes:
#   cold     ext4 page cache + fillmem: cold pages dominate -> branch 7
#            (full scan) or branch 6 (young working set)
#   y80      forced young/cold 80/20 -> branch 6, >=40% scan suppression
#   t5       forced refault distance 5 -> branch 1, 100% file suppression
#   ref      MADV_PAGEOUT + refault of 128 KiB -> branch 2 (d > 32)
#   restore  patch on -> restore -> rmmod -> proc/module gone
#
# Env: KB_IMAGE, KB_KO (required), KB_MEM (512), BUSYBOX, QEMU, CC.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}
KB_MEM=${KB_MEM:-512}
CC=${CC:-clang}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ktramp-stagee.XXXXXX")

# --- workload image: real ext4 file through loop0 -------------------------
truncate -s 40M "$TMP/disk.img"
mke2fs -t ext4 -F -q -b 4096 "$TMP/disk.img" >/dev/null 2>&1
truncate -s 32M "$TMP/bigfile"
debugfs -w -R "write $TMP/bigfile big" "$TMP/disk.img" >/dev/null 2>&1

"$CC" -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
      -Wl,-e,_start -o "$TMP/fillmem" "$HERE/fillmem.c"
"$CC" -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
      -Wl,-e,_start -o "$TMP/faultout" "$HERE/faultout.c"
chmod 755 "$TMP/fillmem" "$TMP/faultout"

run_one() { # label mode ref
	local label=$1 mode=$2 ref=$3
	local root="$TMP/root-$label" log="$TMP/serial-$label.log"
	mkdir -p "$root"/{bin,proc,sys,dev,tmp,module,mnt}
	cp -f "$BUSYBOX" "$root/bin/busybox"; chmod 755 "$root/bin/busybox"
	for a in sh mount insmod rmmod cat echo dmesg grep sleep mknod dd mkdir sync losetup sed tail; do
		ln -sf busybox "$root/bin/$a"
	done
	cp -f "$KB_KO" "$root/module/ktramp.ko"
	cp -f "$TMP/disk.img" "$root/disk.img"
	cp -f "$TMP/fillmem" "$root/fillmem"; chmod 755 "$root/fillmem"
	cp -f "$TMP/faultout" "$root/faultout"; chmod 755 "$root/faultout"
	cat > "$root/init" <<INIT
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp
for x in 'null 1 3' 'zero 1 5' 'console 5 1' 'kmsg 1 11'; do set -- \$x; mknod /dev/\$1 c \$2 \$3 2>/dev/null; done
mknod /dev/loop0 b 7 0 2>/dev/null
echo "=== KTRAMP $label ==="
insmod /module/ktramp.ko; echo "insmod_rc=\$?"
losetup /dev/loop0 /disk.img; echo "loop_rc=\$?"
if [ "$ref" = 1 ]; then mount -t ext4 /dev/loop0 /mnt; else mount -t ext4 -o ro /dev/loop0 /mnt; fi
echo "mount_rc=\$?"
cat /mnt/big > /dev/null
if [ "$ref" = 1 ]; then ./faultout /mnt/big 0 128; fi
echo "$mode" > /proc/ktramp; echo "mode_rc=\$?"
./fillmem 345 &
i=0
while [ \$i -lt 120 ]; do
  C=\$(cat /proc/ktramp | sed -n 's/.*nr_calls=\([0-9]*\).*/\1/p')
  [ "\${C:-0}" -ge 20 ] && break
  sleep 1; i=\$((i+1))
done
echo "poll=\$i nr_calls=\${C:-0}"
echo "=== COUNTERS ==="
cat /proc/ktramp
if [ "$label" = "restore" ]; then
  echo 0 > /proc/ktramp; echo "restore_rc=\$?"
  cat /proc/ktramp
  rmmod ktramp; echo "rmmod_rc=\$?"
  [ -f /proc/ktramp ] && echo proc_still_present || echo proc_gone
  grep -q '^ktramp ' /proc/modules && echo module_still_loaded || echo module_gone
fi
echo "=== DONE ==="
while true; do sleep 5; done
INIT
	chmod 755 "$root/init"
	( cd "$root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs-$label.cpio.gz" )
	timeout 150 "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" \
		-accel tcg,thread=single -kernel "$KB_IMAGE" \
		-initrd "$TMP/initramfs-$label.cpio.gz" \
		-append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" \
		-nographic -no-reboot > "$log" 2>&1 || true
	sed -n '/=== COUNTERS ===/,/=== DONE ===/p' "$log" | tr -d '\r' > "$TMP/counters-$label.txt"
}

field() { # label field — exact key match (no substring collisions like ttl vs minttl)
	awk -v k="$2" '{for(i=1;i<=NF;i++){if(split($i,a,"=")==2 && a[1]==k){print a[2]; exit}}}' "$TMP/counters-$1.txt"
}
branch() { # label index
	local b; b=$(field "$1" branch)
	[ -n "$b" ] && printf '%s' "$b" | cut -d, -f"$2" || echo 0
}
fail=0
check() { if [ "$1" = 1 ]; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }

# 1. cold working set: policy must run and take a generation-tier branch.
run_one cold 2 0
cold_policy=$(field cold policy); cold_calls=$(field cold nr_calls)
cold_b6=$(branch cold 6); cold_b7=$(branch cold 7)
check "$([ "$cold_policy" = 1 ] && [ "${cold_calls:-0}" -gt 0 ] && [ $((cold_b6 + cold_b7)) -gt 0 ] && echo 1 || echo 0)" \
      "cold: policy=1 nr_calls>0 branch6/7 ($cold_b6/$cold_b7)"
echo "COUNTERS cold: $(grep "ktramp target=" "$TMP/counters-cold.txt" | head -1)"

# 2. forced young working set: branch 6 and >=40% suppression.
run_one y80 y80 0
y_b6=$(branch y80 6); y_pre=$(field y80 pre20); y_delta=$(field y80 delta20)
y_ok=0
if [ "${y_b6:-0}" -gt 0 ] && [ "${y_pre:-0}" -gt 0 ] && [ $((y_delta * 100)) -ge $((y_pre * 40)) ]; then y_ok=1; fi
check "$y_ok" "y80: branch6=$y_b6 delta20=$y_delta pre20=$y_pre (>=40%)"

# 3. forced small refault distance: branch 1 and 100% file suppression.
run_one t5 t5 0
t_b1=$(branch t5 1); t_pre=$(field t5 pre20); t_delta=$(field t5 delta20)
t_ok=0
if [ "${t_b1:-0}" -gt 0 ] && [ "${t_pre:-0}" -gt 0 ] && [ "$t_delta" = "$t_pre" ]; then t_ok=1; fi
check "$t_ok" "t5: branch1=$t_b1 delta20=$t_delta pre20=$t_pre (100%)"

# 4. forced eviction-age split: old -> full scan, young -> protect.
run_one a80 a80 0
a8=$(branch a80 8)
check "$([ "${a8:-0}" -gt 0 ] && echo 1 || echo 0)" "a80: branch8=$a8 (old evictions -> full scan)"
run_one a10 a10 0
a9=$(branch a10 9); a_pre=$(field a10 pre20); a_delta=$(field a10 delta20)
a_ok=0
if [ "${a9:-0}" -gt 0 ] && [ "${a_pre:-0}" -gt 0 ] && [ $((a_delta * 100)) -ge $((a_pre * 40)) ]; then a_ok=1; fi
check "$a_ok" "a10: branch9=$a9 delta20=$a_delta pre20=$a_pre (>=40%)"

# 5. real refault (MADV_PAGEOUT of 128 KiB): branch 2.
run_one ref 2 1
r_b2=$(branch ref 2); r_td=$(field ref tdist); r_ttl=$(field ref ttl); r_age=$(field ref age)
check "$([ "${r_b2:-0}" -gt 0 ] && [ -n "$r_ttl" ] && [ -n "$r_age" ] && echo 1 || echo 0)" \
      "ref: branch2=$r_b2 tdist=$r_td ttl=$r_ttl age=$r_age"

# 6. restore + unload.
run_one restore 1 0
r_patched=$(sed -n 's/.*patched=\([0-9]*\).*/\1/p' "$TMP/counters-restore.txt" | tail -1)
r_rmmod=$(field restore rmmod_rc)
r_proc=$(grep -c '^proc_gone$' "$TMP/counters-restore.txt" || true)
r_mod=$(grep -c '^module_gone$' "$TMP/counters-restore.txt" || true)
check "$([ "$r_patched" = 0 ] && [ "$r_rmmod" = 0 ] && [ "$r_proc" = 1 ] && [ "$r_mod" = 1 ] && echo 1 || echo 0)" \
      "restore: patched=$r_patched rmmod_rc=$r_rmmod proc_gone=$r_proc module_gone=$r_mod"

echo "logs: $TMP"
[ "$fail" = 0 ] && echo "PASS ktramp Stage E gate" || { echo "FAIL ktramp Stage E gate"; exit 1; }
