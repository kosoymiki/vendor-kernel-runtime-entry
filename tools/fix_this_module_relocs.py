#!/usr/bin/env python3
"""Patch .rela.gnu.linkonce.this_module init/exit offsets by symbol name.

Vendor kernels may have a different `struct module` layout than the local
build tree. Never patch by a hardcoded raw offset: match the relocation
symbols (`init_module`, `cleanup_module`) and write the offsets observed in a
known-good vendor module / QEMU-gdb gate.

Usage: fix_this_module_relocs.py [--init-off 0x190] [--exit-off 0x3c0] <ko>
"""
import argparse
import struct
import sys


def section_headers(d):
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize = struct.unpack_from("<H", d, 0x3A)[0]
    shnum = struct.unpack_from("<H", d, 0x3C)[0]
    shstrndx = struct.unpack_from("<H", d, 0x3E)[0]
    str_off = struct.unpack_from("<Q", d, shoff + shstrndx * shentsize + 0x18)[0]
    str_size = struct.unpack_from("<Q", d, shoff + shstrndx * shentsize + 0x20)[0]
    return shoff, shentsize, shnum, d[str_off:str_off + str_size]


def patch(path, init_off, exit_off):
    d = bytearray(open(path, "rb").read())
    shoff, es, n, names = section_headers(d)
    target = None
    for i in range(n):
        o = shoff + i * es
        name_off = struct.unpack_from("<I", d, o)[0]
        name = names[name_off:names.find(b"\0", name_off)]
        if name == b".rela.gnu.linkonce.this_module":
            target = o
            break
    if target is None:
        print("FAIL no .rela.gnu.linkonce.this_module in %s" % path)
        return 1
    rel_off = struct.unpack_from("<Q", d, target + 0x18)[0]
    rel_size = struct.unpack_from("<Q", d, target + 0x20)[0]
    symtab_idx = struct.unpack_from("<I", d, target + 0x28)[0]
    sym_o = shoff + symtab_idx * es
    sym_off = struct.unpack_from("<Q", d, sym_o + 0x18)[0]
    sym_str_idx = struct.unpack_from("<I", d, sym_o + 0x28)[0]
    sstr_o = shoff + sym_str_idx * es
    sstr_off = struct.unpack_from("<Q", d, sstr_o + 0x18)[0]
    sstr_size = struct.unpack_from("<Q", d, sstr_o + 0x20)[0]
    sstr = d[sstr_off:sstr_off + sstr_size]
    want = {b"init_module": init_off, b"cleanup_module": exit_off}
    patched = {}
    for j in range(rel_size // 24):
        ro = rel_off + j * 24
        info = struct.unpack_from("<Q", d, ro + 8)[0]
        sym_idx = info >> 32
        so = sym_off + sym_idx * 24
        st_name = struct.unpack_from("<I", d, so)[0]
        sym = bytes(sstr[st_name:sstr.find(b"\0", st_name)])
        if sym not in want:
            continue
        cur = struct.unpack_from("<Q", d, ro)[0]
        if cur != want[sym]:
            struct.pack_into("<Q", d, ro, want[sym])
        patched[sym.decode()] = (cur, want[sym])
    if b"init_module" not in {k.encode() for k in patched}:
        print("FAIL %s missing init_module relocation" % path)
        return 1
    open(path, "wb").write(d)
    for sym, (cur, target) in sorted(patched.items()):
        print("reloc %-15s 0x%x -> 0x%x %s" % (sym, cur, target, "ok" if cur == target else "fixed"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--init-off", type=lambda x: int(x, 0), default=0x190)
    ap.add_argument("--exit-off", type=lambda x: int(x, 0), default=0x3c0)
    ap.add_argument("module")
    a = ap.parse_args()
    return patch(a.module, a.init_off, a.exit_off)


if __name__ == "__main__":
    sys.exit(main())
