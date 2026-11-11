# thp_enable / thpfill — THP diagnostics (not release artifacts)

> Part of a library under **active development**. These are test helpers for
> the folio investigation, not shipped modules.

`thpfill` mmaps N MiB with `MADV_HUGEPAGE` and touches it; `thp_enable` sets
the `transparent_hugepage_flags` madvise bit directly, because the vendor
kernel compiles out `enabled_store()` under `CONFIG_CONT_PTE_HUGEPAGE`
(`mm/huge_memory.c:204`, `#ifndef CONFIG_CONT_PTE_HUGEPAGE`).

Observed on the QEMU vendor Image: writing the resolved flag symbol did not
change the sysfs-visible value (`[always]` persisted) and `AnonHugePages`
stayed `0 kB`; `CONFIG_CONT_PTE_HUGEPAGE_ON_QEMU` is not set. Compound pages
are therefore not produced by this image, so the QEMU folio proof is not
available; the module-side folio head normalization is unit-tested instead.
The full evidence is recorded in `docs/VMSSCAN-HOOK.md` and the H2 capture.
