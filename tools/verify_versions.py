#!/usr/bin/env python3
"""Hard gate: verify a module's __versions table against the stock Image.

Why this exists
---------------
The vendor kernel is built with CONFIG_MODVERSIONS=y and
# CONFIG_MODULE_FORCE_LOAD is not set.  In kernel/module.c:

    if (versindex == 0)                       /* section INDEX, not size */
        return try_to_force_load(mod, symname) == 0;

and try_to_force_load() is literally `return -ENOEXEC;` when force-load is
disabled.  So a module WITHOUT a __versions section dies with a silent
-ENOEXEC ("Exec format error") before any version diagnostic is printed.
A module WITH an empty __versions section dies later with "no symbol version".

Both are fatal.  The only loadable shape is a __versions table whose entries
match the running kernel's __kcrctab exactly.

Ground truth
------------
__ksymtab/__kcrctab pairs decoded from the stock Image.  Validated against
three vendor modules that provably load on this kernel: 175/175 CRCs exact
(aw8697 91/91, bcl_soc 21/21, btpower 63/63).

Checks (all fatal)
------------------
 1. __versions section exists.
 2. sh_size % 64 == 0 and entry count > 0.
 3. Every SHN_UNDEF symbol of the module has an entry.
 4. Every entry's CRC equals the Image CRC for that symbol.
 5. Every entry's symbol is actually exported by the Image (has __ksymtab_).
 6. module_layout is present (the kernel always checks it).

Usage:
  verify_versions.py --image IMG --kallsyms KS MODULE.ko [MODULE.ko ...]
Exit code 0 only when every module passes every check.
"""
import argparse
import struct
import sys

LINK_BASE = 0xFFFFFFEA26400000
MODVERSION_ENTRY_SIZE = 64          # unsigned long crc + char name[56]
MODULE_NAME_LEN = 56


def parse_kallsyms(path):
    d = {}
    with open(path, errors="replace") as fh:
        for line in fh:
            p = line.split()
            if len(p) >= 3:
                try:
                    a = int(p[0], 16)
                except ValueError:
                    continue
                d.setdefault(p[2], []).append(a)
    return d


class ImageExports:
    """name -> (group, crc) for every symbol exported by the stock Image."""

    def __init__(self, image_path, kallsyms_path):
        ks = parse_kallsyms(kallsyms_path)
        self.base = ks["_text"][0]
        with open(image_path, "rb") as fh:
            self.blob = fh.read()

        def la(a):
            return a - self.base + LINK_BASE

        self.sections = [
            ("normal",
             la(ks["__start___ksymtab"][0]), la(ks["__stop___ksymtab"][0]),
             la(ks["__start___kcrctab"][0])),
            ("gpl",
             la(ks["__start___ksymtab_gpl"][0]), la(ks["__stop___ksymtab_gpl"][0]),
             la(ks["__start___kcrctab_gpl"][0])),
        ]
        self.ksymtab_addrs = {}
        for name, addrs in ks.items():
            if name.startswith("__ksymtab_"):
                self.ksymtab_addrs[name[len("__ksymtab_"):]] = addrs

    def crc_for(self, name):
        """Return (group, crc) or (None, None) when not exported."""
        for raw in self.ksymtab_addrs.get(name, ()):
            a = raw - self.base + LINK_BASE
            for group, lo, hi, crc_lo in self.sections:
                if lo <= a < hi:
                    off = crc_lo + ((a - lo) // 12) * 4 - LINK_BASE
                    return group, struct.unpack_from("<I", self.blob, off)[0]
        return None, None


def elf_sections(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("FAIL %s: not little-endian ELF64" % path)
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    def sh(i):
        o = e_shoff + i * e_shentsize
        f = struct.unpack_from("<IIQQQQIIQQ", data, o)
        return dict(zip(("name", "type", "flags", "addr", "off", "size",
                         "link", "info", "align", "entsize"), f))

    shstr = sh(e_shstrndx)
    st = data[shstr["off"]:shstr["off"] + shstr["size"]]
    secs = {}
    ordered = []
    for i in range(e_shnum):
        s = sh(i)
        s["name_s"] = st[s["name"]:st.find(b"\0", s["name"])].decode()
        ordered.append(s)
        secs[s["name_s"]] = s
    return data, secs, ordered


def undefined_symbols(data, secs, ordered):
    """Names of SHN_UNDEF symbols, excluding the null symbol and weak ones."""
    symtab = secs.get(".symtab")
    if symtab is None:
        raise SystemExit("FAIL: no .symtab (module was fully stripped)")
    strtab = ordered[symtab["link"]]
    names = []
    count = symtab["size"] // symtab["entsize"]
    for i in range(1, count):
        off = symtab["off"] + i * symtab["entsize"]
        st_name, st_info, st_other, st_shndx = struct.unpack_from("<IBBH", data, off)
        if st_shndx != 0:          # SHN_UNDEF
            continue
        end = data.find(b"\0", strtab["off"] + st_name)
        name = data[strtab["off"] + st_name:end].decode("ascii", "replace")
        if name:
            names.append((name, st_info & 0xF))
    return names


def verify(path, exports, verbose=True):
    data, secs, ordered = elf_sections(path)
    label = path.rsplit("/", 1)[-1]
    errors = []

    vers = secs.get("__versions")
    if vers is None:
        errors.append("__versions section MISSING -> kernel returns silent -ENOEXEC "
                      "(CONFIG_MODULE_FORCE_LOAD=n)")
        return errors, {}
    if vers["size"] == 0:
        errors.append("__versions is EMPTY -> every symbol fails 'no symbol version'")
        return errors, {}
    if vers["size"] % MODVERSION_ENTRY_SIZE:
        errors.append("__versions size %d is not a multiple of %d"
                      % (vers["size"], MODVERSION_ENTRY_SIZE))
        return errors, {}

    entries = {}
    count = vers["size"] // MODVERSION_ENTRY_SIZE
    for i in range(count):
        off = vers["off"] + i * MODVERSION_ENTRY_SIZE
        crc = struct.unpack_from("<Q", data, off)[0]
        raw = data[off + 8:off + MODVERSION_ENTRY_SIZE]
        name = raw.split(b"\0")[0].decode("ascii", "replace")
        if crc > 0xFFFFFFFF:
            errors.append("entry %d (%s): CRC %#x does not fit in 32 bits" % (i, name, crc))
        entries[name] = crc

    undef = undefined_symbols(data, secs, ordered)
    undef_names = [n for n, _ in undef]
    missing = [n for n in undef_names if n not in entries]
    for name in missing:
        errors.append("import %s has NO __versions entry" % name)

    mismatched = 0
    not_exported = []
    for name, crc in sorted(entries.items()):
        group, real = exports.crc_for(name)
        if real is None:
            not_exported.append(name)
            errors.append("entry %s is NOT exported by the Image (no __ksymtab_)" % name)
            continue
        if crc & 0xFFFFFFFF != real:
            mismatched += 1
            errors.append("entry %s CRC %#010x != Image %#010x"
                          % (name, crc & 0xFFFFFFFF, real))

    if "module_layout" not in entries:
        errors.append("module_layout entry missing (kernel always checks it)")

    summary = {
        "module": label,
        "entries": count,
        "imports": len(undef_names),
        "mismatched": mismatched,
        "not_exported": len(not_exported),
    }
    if verbose:
        print("%-24s entries=%-4d imports=%-3d crc_bad=%-3d not_exported=%d"
              % (label, count, len(undef_names), mismatched, len(not_exported)))
    return errors, summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--kallsyms", required=True)
    ap.add_argument("modules", nargs="+")
    args = ap.parse_args()

    exports = ImageExports(args.image, args.kallsyms)
    rc = 0
    for mod in args.modules:
        try:
            errors, _ = verify(mod, exports)
        except FileNotFoundError:
            print("FAIL %s: not found" % mod)
            rc = 1
            continue
        if errors:
            rc = 1
            print("FAIL %s" % mod)
            for e in errors[:40]:
                print("   -", e)
            if len(errors) > 40:
                print("   ... %d more" % (len(errors) - 40))
        else:
            print("PASS %s" % mod)
    return rc


if __name__ == "__main__":
    sys.exit(main())
