# Examples / extending the base module

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

The reference module is intentionally small. To run your own trusted code:

1. Copy `kernel/kbootstrap_core.c` as a starting point.
2. Keep the `module_init`/`module_exit` pair and the CFI bridge objects.
3. Resolve symbols with `register_kprobe` (see `sym()` in the reference) rather
   than relying on exports.
4. If you allocate executable memory: write → `__flush_icache_range` →
   `set_memory_x(page, 1)` → flush again → call.
5. Expose status through a `proc_ops` read handler or module parameters; avoid
   unproven `seq_file`/function-pointer paths.
6. Build with `tools/adapt_kernel.sh`; run `tests/qemu_payload_gate.sh` with
   the exact Image before touching a device.
7. Never `rmmod` on a device until the unload/CFI path is proven in QEMU.

The base module is not a general-purpose bytecode loader. “Arbitrary” here
means: a module built for the target ABI can execute trusted kernel code.
Every payload is target-specific and must pass the same gates.
