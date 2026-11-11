#!/usr/bin/env bash
# ktramp Stage K gate: prove the per-memcg resident generation histogram and
# the min_seq/window clock on the real reclaim path.
#
# Two boots:
#   cold  ext4 page cache + fillmem: every tracked page is resident -> the
#         histogram must be non-empty, min_seq <= seq and min_seq >= seq-3
#         (min_seq is derived from the oldest non-empty bucket, so it can only
#         trail seq by 0..3; the earlier "min never advanced" bug produced
#         min_seq=0 with seq=65 and is caught here).
#   ref   MADV_PAGEOUT + refault of 128 KiB: evictions decrement their bucket,
#         the clock stays consistent, refault branch still fires.
#
# Env: KB_IMAGE, KB_KO (required), KB_MEM (512), BUSYBOX, QEMU, CC.
set -euo pipefail
KB_IMAGE=${KB_IMAGE:?}; KB_KO=${KB_KO:?}
KB_MEM=${KB_MEM:-512}
CC=${CC:-clang}
BUSYBOX=${BUSYBOX:-$HOME/ae-qemu-gate/bin/busybox}; QEMU=${QEMU:-qemu-system-aarch64}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ktramp-stagek.XXXXXX")

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
while [ \$i -lt 200 ]; do
  C=\$(cat /proc/ktramp | sed -n 's/.*nr_calls=\([0-9]*\).*/\1/p')
  [ "\${C:-0}" -ge 10 ] && break
  sleep 1; i=\$((i+1))
done
echo "poll=\$i nr_calls=\${C:-0}"
echo "=== COUNTERS ==="
cat /proc/ktramp
echo "=== DONE ==="
while true; do sleep 5; done
INIT
	chmod 755 "$root/init"
	( cd "$root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$TMP/initramfs-$label.cpio.gz" )
	timeout 240 "$QEMU" -M virt -cpu cortex-a72 -smp 1 -m "$KB_MEM" \
		-accel tcg,thread=single -kernel "$KB_IMAGE" \
		-initrd "$TMP/initramfs-$label.cpio.gz" \
		-append "console=ttyAMA0 nokaslr rdinit=/init panic=-1 loglevel=7 maxcpus=1" \
		-nographic -no-reboot > "$log" 2>&1 || true
	sed -n '/=== COUNTERS ===/,/=== DONE ===/p' "$log" | tr -d '\r' > "$TMP/counters-$label.txt"
}

field() { # label field — exact key match (no substring collisions like ttl vs minttl)
	awk -v k="$2" '{for(i=1;i<=NF;i++){if(split($i,a,"=")==2 && a[1]==k){print a[2]; exit}}}' "$TMP/counters-$1.txt"
}
fail=0
check() { if [ "$1" = 1 ]; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }

# Stage K invariants for one boot.
stagek() { # label
	local label=$1 seq min win res rsum=0 r
	seq=$(field "$label" seq); min=$(field "$label" min); win=$(field "$label" win)
	res=$(field "$label" res)
	[ -n "$res" ] && for r in $(printf '%s' "$res" | tr ',' ' '); do rsum=$((rsum + r)); done
	echo "STAGEK $label: seq=$seq min=$min win=$win res=$res res_sum=$rsum"
	local ok=0
	if [ -n "$seq" ] && [ -n "$min" ] && [ -n "$win" ] && [ -n "$res" ] \
	   && [ "$seq" -gt 0 ] && [ "$rsum" -gt 0 ] \
	   && [ "$min" -le "$seq" ] && [ "$min" -ge $((seq - 3)) ] \
	   && [ "$win" -ge 1 ] && [ "$win" -ge $((seq - min)) ]; then ok=1; fi
	check "$ok" "$label: histogram non-empty, min_seq in [seq-3,seq], win>=seq-min"
	# Aging proof: with bucket shifting a resident page that was not touched for
	# 3+ epochs lands in bucket 3, so min_seq must trail seq. The pre-fix
	# degenerate histogram could only ever produce min_seq == seq.
	if [ "${2:-}" = strict ]; then
		check "$([ -n "$min" ] && [ "$min" -lt "$seq" ] && echo 1 || echo 0)" \
		      "$label: min<seq (generation aging active)"
	fi
}

run_one cold 2 0
stagek cold strict
check "$([ "$(field cold policy)" = 1 ] && echo 1 || echo 0)" "cold: policy=1"
run_one ref 2 1
stagek ref
check "$([ "$(field ref branch | cut -d, -f2)" -gt 0 ] && echo 1 || echo 0)" \
      "ref: refault branch2=$(field ref branch | cut -d, -f2) tdist=$(field ref tdist)"

echo "logs: $TMP"
[ "$fail" = 0 ] && echo "PASS ktramp Stage K gate" || { echo "FAIL ktramp Stage K gate"; exit 1; }
