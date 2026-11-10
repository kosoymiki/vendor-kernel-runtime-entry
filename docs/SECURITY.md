# Security and risk

## What this gives you

The reference module executes arbitrary trusted code in kernel context. That
is the point: it is a runtime entry/payload bus for a device you own.

## Risks

- A bug in the module or payload can panic, corrupt kernel memory, or brick the
  device.
- The CFI bridge (`__cfi_check: ret`, jump tables) **disables CFI checks for
  this module**. It does not disable CFI kernel-wide, but module code is
  unchecked. Only load code you built/audited.
- `rmmod` may panic if the unload/CFI path was not proven. Do not unload on a
  device; reboot is the rollback.
- Vendor kernels differ. Wrong offsets, CRCs or vermagic can fail at load or
  corrupt state.
- Root/KernelSU access is required. Treat the module as privileged code.

## Safe operating procedure

1. Back up boot/vendor_boot and know how to restore them.
2. Use a spare/test device where possible.
3. Validate in QEMU with the exact `Image` and `nokaslr` first.
4. Keep the module loaded; do not `rmmod`. Reboot to roll back.
5. Record hashes/commands/logs for every build.
6. Never load a module built for a different profile/device.

## Out of scope / prohibited use

This project is for the operator's own device and research. It must not be used
to bypass third-party authentication, DRM, CAPTCHA, paywalls, or to attack
systems you do not own. No CVE exploit is included. Follow local law and the
terms that apply to your device.

## Supply chain

Build from source with the target tree; keep `KB_IMAGE`, `kallsyms`, vendor
`.ko` and `Module.symvers` provenance. Do not trust a prebuilt `.ko` from an
unknown source. The repository's `tests/static_checks.sh` checks for obvious
secret-like strings but is not a security audit.

## Disclosure

If you find a bug in this project, open an issue with the exact build profile,
logs and hashes. If you find a vulnerability in a vendor kernel, follow the
vendor's disclosure process; this project does not provide exploit payloads.
