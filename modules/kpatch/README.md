# kpatch — text-patch probe

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

Proof that the entry can replace a live kernel function and restore it:

- resolve `aarch64_insn_patch_text_nosync`, `__arm64_sys_getpid`, kallsyms and
  proc helpers through kprobe;
- save the first two instructions of `__arm64_sys_getpid`;
- patch to `mov w0,#0; ret` via `/proc/kpatch` write `1`;
- verify `getpid()` returns 0;
- restore original instructions via write `0`.

Artifact: `kpatch.ko` 1.0.2, sha256
`c4f9a319feb6da9d8989bbdb15903bcb6ccce4c361c1fcd1edfc237dfe5ddf11`, gated by
`tests/qemu_probe_gate.sh`.

**Reference evidence (QEMU, exact Image):** before `getpid=114`, patched `0`,
restored `118` (`$PREFIX/tmp/kpatch2-1794330412/q.log`). This is a probe, not a
stable patch framework.

## Safety

`aarch64_insn_patch_text_nosync` does not stop other CPUs. It is used here on a
single-CPU QEMU. On a live SMP device use a stop_machine/text-patch API and
verify under load; do not load this probe on a device as-is. A wrong patch can
panic or corrupt execution. Keep a restore path; never rmmod a module whose
text patch was not restored.

## Build (reference profile)

```sh
KB_MODULE_SRC=$PWD/modules/kpatch KB_MODNAME=kpatch \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
```
