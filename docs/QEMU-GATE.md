# QEMU gate

The QEMU gate boots the **exact stock Image** with a minimal initramfs,
loads the module, and asserts the side effect. It does not write to the device.

## Requirements

- `qemu-system-aarch64`
- statically linked busybox (`BUSYBOX`, default `~/ae-qemu-gate/bin/busybox`)
- exact uncompressed `Image` (`KB_IMAGE`)
- built module (`KB_KO`)

## Run

```sh
KB_IMAGE=$HOME/tmp/kernel_Image.bin \
KB_KO=$PWD/kernel/kbootstrap.ko \
BUSYBOX=$HOME/ae-qemu-gate/bin/busybox \
tests/qemu_payload_gate.sh
```

## What it asserts

- `insmod_rc=0`
- module appears in `/proc/modules`
- `/proc/kbootstrap` exists and contains `payload_result=42`
- no CFI failure at init (the CFI bridge warning “not using shadow” is expected)

## What it does not prove

- device-specific hardware behavior;
- unload/`rmmod` safety;
- that a different kernel/profile is compatible.

For unload, use a separate QEMU experiment with gdb and a proven `mod->exit`
path; do not `rmmod` on a device for the first time.

## Reference result

```
cfi: not using shadow for module kbootstrap
insmod_rc=0
kbootstrap: ready kln=... alloc=... flush=... sct=... payload=42
payload_result=42
proc_entry=created
```

See `docs/EVIDENCE.md`.
