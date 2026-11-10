#!/system/bin/sh
# KernelSU/Magisk installer hook.
MODDIR=${0%/*}
chmod 0755 "$MODDIR/post-fs-data.sh" "$MODDIR/service.sh" "$MODDIR/uninstall.sh" 2>/dev/null
chmod 0644 "$MODDIR/bin/kbootstrap.ko" 2>/dev/null
