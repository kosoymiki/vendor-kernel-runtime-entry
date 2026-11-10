#!/usr/bin/env python3
"""Reconstruct Module.symvers CRCs for the stock kernel from its own Image.

Ground truth chain:
  /proc/kallsyms (runtime, root)  ->  link-time address via _text delta
  Image file at (link_addr - LINK_BASE)  ->  __ksymtab entry / __kcrctab slot

Layout facts for this build (verified in-tree):
  struct kernel_symbol is 3 x s32 (CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y)
  CONFIG_MODULE_REL_CRCS is not set  =>  __kcrctab slot holds an absolute u32 CRC
  __ksymtab and __kcrctab are index-paired (SORT(___ksymtab+*) / SORT(___kcrctab+*))

Usage:
  symvers_from_image.py --image <Image> --kallsyms <dump> --symbols <file|-> \
      [--out Module.symvers] [--selfcheck]
"""
import argparse
import struct
import sys

LINK_BASE_DEFAULT = 0xFFFFFFEA26400000  # _head == _text of this Image (r2-verified)


def parse_kallsyms(path):
    """name -> list of addresses (a name can exist in vmlinux and in a module)."""
    syms = {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            syms.setdefault(parts[2], []).append(addr)
    return syms


def one(syms, name):
    addrs = syms.get(name)
    if not addrs:
        raise SystemExit("missing kallsyms symbol: %s" % name)
    return addrs[0]


def link_addr(runtime, runtime_base, link_base):
    return runtime - runtime_base + link_base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--kallsyms", required=True)
    ap.add_argument("--symbols", required=True, help="file with one symbol per line, or - for stdin")
    ap.add_argument("--out", default="-")
    ap.add_argument("--link-base", type=lambda x: int(x, 0), default=LINK_BASE_DEFAULT)
    ap.add_argument("--selfcheck", action="store_true")
    args = ap.parse_args()

    ks = parse_kallsyms(args.kallsyms)
    needed = [
        "_text", "_head",
        "__start___ksymtab", "__stop___ksymtab",
        "__start___kcrctab", "__stop___kcrctab",
        "__start___ksymtab_gpl", "__stop___ksymtab_gpl",
        "__start___kcrctab_gpl", "__stop___kcrctab_gpl",
        "__start___ksymtab_gpl_future", "__stop___ksymtab_gpl_future",
        "__start___kcrctab_gpl_future", "__stop___kcrctab_gpl_future",
        "__start___ksymtab_unused", "__stop___ksymtab_unused",
        "__start___kcrctab_unused", "__stop___kcrctab_unused",
        "__start___ksymtab_unused_gpl", "__stop___ksymtab_unused_gpl",
        "__start___kcrctab_unused_gpl", "__stop___kcrctab_unused_gpl",
    ]
    missing = [s for s in needed if s not in ks]
    if missing:
        sys.exit("missing kallsyms bounds symbols: %s" % ",".join(missing))

    runtime_base = one(ks, "_text")
    L = args.link_base

    def la(name):
        return link_addr(one(ks, name), runtime_base, L)

    blob = open(args.image, "rb").read()

    def rd32(link):
        off = link - L
        if off < 0 or off + 4 > len(blob):
            raise SystemExit("read out of range for link addr 0x%x (off 0x%x, size 0x%x)"
                             % (link, off, len(blob)))
        return struct.unpack_from("<I", blob, off)[0]

    def rd32s(link):
        v = rd32(link)
        return v - (1 << 32) if v & 0x80000000 else v

    def cstr(link):
        off = link - L
        end = blob.find(b"\0", off)
        if off < 0 or end < 0:
            raise SystemExit("bad string link addr 0x%x" % link)
        return blob[off:end].decode("ascii", "replace")

    groups = [
        ("normal", "__start___ksymtab", "__stop___ksymtab",
         "__start___kcrctab", "__stop___kcrctab"),
        ("gpl", "__start___ksymtab_gpl", "__stop___ksymtab_gpl",
         "__start___kcrctab_gpl", "__stop___kcrctab_gpl"),
        ("gpl_future", "__start___ksymtab_gpl_future", "__stop___ksymtab_gpl_future",
         "__start___kcrctab_gpl_future", "__stop___kcrctab_gpl_future"),
        ("unused", "__start___ksymtab_unused", "__stop___ksymtab_unused",
         "__start___kcrctab_unused", "__stop___kcrctab_unused"),
        ("unused_gpl", "__start___ksymtab_unused_gpl", "__stop___ksymtab_unused_gpl",
         "__start___kcrctab_unused_gpl", "__stop___kcrctab_unused_gpl"),
    ]
    table = []
    for gname, ks_lo, ks_hi, cr_lo, cr_hi in groups:
        lo, hi = la(ks_lo), la(ks_hi)
        clo, chi = la(cr_lo), la(cr_hi)
        n = (hi - lo) // 12
        cn = (chi - clo) // 4
        if n != cn:
            print("WARN group %s: ksymtab entries=%d kcrctab entries=%d" % (gname, n, cn),
                  file=sys.stderr)
        table.append((gname, lo, hi, clo, n))
        if args.selfcheck:
            print("group %-11s ksymtab=[0x%x,0x%x) n=%d crctab=0x%x" % (gname, lo, hi, n, clo),
                  file=sys.stderr)

    def crc_for(name):
        ksym = "__ksymtab_" + name
        for raw in ks.get(ksym, ()):
            a = link_addr(raw, runtime_base, L)
            for gname, lo, hi, clo, n in table:
                if lo <= a < hi:
                    idx = (a - lo) // 12
                    if (a - lo) % 12:
                        raise SystemExit("unaligned __ksymtab_%s" % name)
                    return gname, rd32(clo + idx * 4)
        return None, None

    # Structural self-check: __ksymtab entry must point at its own name string.
    if args.selfcheck:
        for probe in ("module_layout", "printk", "register_kprobe", "kfree"):
            gname, crc = crc_for(probe)
            if gname is None:
                print("selfcheck: %s NOT EXPORTED" % probe, file=sys.stderr)
                continue
            a = la("__ksymtab_" + probe)
            voff, noff, nsoff = struct.unpack_from("<iii", blob, a - L)
            nm = cstr(a + 4 + noff)
            ok = "OK" if nm == probe else "MISMATCH(%s)" % nm
            print("selfcheck: %-16s group=%-8s crc=0x%08x name=%s" % (probe, gname, crc, ok),
                  file=sys.stderr)

    src = sys.stdin if args.symbols == "-" else open(args.symbols, "r")
    out = sys.stdout if args.out == "-" else open(args.out, "w")
    missing_syms, zero = [], []
    count = 0

    # module_layout is not referenced by any module's ELF symbol table, but
    # modpost injects it into every module's unresolved list and the kernel's
    # check_modstruct_version() always verifies it.  Without a dump entry
    # modpost silently omits it from __versions and the load fails with
    # "no symbol version for module_layout".
    requested = [line.strip() for line in src
                 if line.strip() and not line.strip().startswith("#")]
    if "module_layout" not in requested:
        requested.insert(0, "module_layout")

    for name in requested:
        gname, crc = crc_for(name)
        if gname is None:
            missing_syms.append(name)
            continue
        if crc == 0:
            zero.append(name)
        export = {"normal": "EXPORT_SYMBOL", "gpl": "EXPORT_SYMBOL_GPL",
                  "gpl_future": "EXPORT_SYMBOL_GPL_FUTURE",
                  "unused": "EXPORT_UNUSED_SYMBOL",
                  "unused_gpl": "EXPORT_UNUSED_SYMBOL_GPL"}[gname]
        out.write("0x%08x\t%s\tvmlinux\t%s\t\n" % (crc, name, export))
        count += 1
    if out is not sys.stdout:
        out.close()
    print("emitted=%d missing=%d zero_crc=%d" % (count, len(missing_syms), len(zero)),
          file=sys.stderr)
    if missing_syms:
        print("MISSING: " + " ".join(missing_syms), file=sys.stderr)
    if zero:
        print("ZERO: " + " ".join(zero), file=sys.stderr)
    return 0 if not missing_syms else 2


if __name__ == "__main__":
    sys.exit(main())
