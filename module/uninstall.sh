#!/system/bin/sh
# Do not rmmod a module whose unload path was not proven; reboot is the safe
# rollback. This script only removes module files.
MODDIR=${0%/*}
echo "H2 Runtime Entry: reboot to complete removal (no rmmod)."
