#!/system/bin/sh
# Load the per-target kbootstrap module. Never rmmod; rollback is reboot.
MODDIR=${0%/*}
KO="$MODDIR/bin/kbootstrap.ko"
LOG="$MODDIR/load.log"
{
  echo "$(date) post-fs-data"
  [ -f "$KO" ] || { echo "missing $KO"; exit 0; }
  insmod "$KO" 2>&1
  echo "insmod_rc=$?"
  [ -r /proc/kbootstrap ] && echo "proc=present" || echo "proc=absent"
} >> "$LOG" 2>&1
