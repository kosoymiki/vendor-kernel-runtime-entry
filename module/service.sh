#!/system/bin/sh
# Status only. The module is intentionally not unloaded at runtime.
MODDIR=${0%/*}
LOG="$MODDIR/service.log"
{
  echo "$(date) service"
  i=0
  while [ "$i" -lt 30 ]; do
    [ -r /proc/kbootstrap ] && break
    sleep 1
    i=$((i + 1))
  done
  if [ -r /proc/kbootstrap ]; then
    echo "proc=present"
    cat /proc/kbootstrap
  else
    echo "proc=absent (check load.log/dmesg)"
  fi
} >> "$LOG" 2>&1
