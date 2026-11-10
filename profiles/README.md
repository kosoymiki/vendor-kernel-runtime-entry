# Target profiles

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

A profile is a shell env file consumed by `tools/adapt_kernel.sh --profile`.
It records the target's exact build inputs and ABI observations. No profile is
universal.

Required values:
- `KB_KERNEL_SRC`, `KB_KERNEL_OUT` — matching source/build tree.
- `KB_IMAGE` — uncompressed `Image` from the device; used for symbol CRCs.
- `KB_KALLSYMS` — `/proc/kallsyms` dump from the running kernel.
- `KB_VERMAGIC` — exact vermagic.
- `KB_INIT_OFF`, `KB_EXIT_OFF` — from a known-good vendor `.ko` or QEMU gdb.
- `KB_LINK_BASE` — only needed when reconstructing `Module.symvers` from Image.
- `KB_MODULE_CFLAGS` — optional; `-DKB_AUDIT_SCT` is reference-specific.

`tools/collect_target.sh` collects config/kallsyms/version. Image extraction
from `boot.img` is device-specific; use your vendor tools and document it.
