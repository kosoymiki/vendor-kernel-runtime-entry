#!/usr/bin/env python3
"""Normalize SHN_XINDEX symbols that ld.lld emitted without a SYMTAB_SHNDX table.

Observed on this toolchain: __UNIQUE_ID_* (and the .modinfo section symbol) get
st_shndx=0xffff while no SHT_SYMTAB_SHNDX section exists. The vendor kernel's
simplify_symbols() indexes sechdrs[st_shndx] and faults (load_module+0x20d4).
The symbols' values/sizes all fall inside .modinfo, so map them to that index.
"""
import struct, sys

SHN_XINDEX = 0xFFFF

def main(path):
    d = bytearray(open(path, 'rb').read())
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', d, 0x3E)[0]
    def sh(i):
        o = e_shoff + i * e_shentsize
        return dict(name=struct.unpack_from('<I', d, o)[0],
                    type=struct.unpack_from('<I', d, o+4)[0],
                    offset=struct.unpack_from('<Q', d, o+0x18)[0],
                    size=struct.unpack_from('<Q', d, o+0x20)[0],
                    link=struct.unpack_from('<I', d, o+0x28)[0],
                    info=struct.unpack_from('<I', d, o+0x2C)[0],
                    entsize=struct.unpack_from('<Q', d, o+0x38)[0])
    shstr = sh(e_shstrndx)
    names = d[shstr['offset']:shstr['offset']+shstr['size']]
    def sname(off):
        end = names.find(b'\0', off)
        return names[off:end].decode('latin1')
    secs = [(i, sname(sh(i)['name']), sh(i)) for i in range(e_shnum)]
    modinfo = [i for i, n, s in secs if n == '.modinfo']
    if not modinfo:
        print('FAIL no .modinfo'); return 1
    modinfo = modinfo[0]
    symtab = [s for i, n, s in secs if n == '.symtab'][0]
    strtab = [s for i, n, s in secs if n == '.strtab'][0]
    strd = d[strtab['offset']:strtab['offset']+strtab['size']]
    entsize = symtab['entsize'] or 24
    fixed = 0
    bad = []
    for off in range(symtab['offset'], symtab['offset']+symtab['size'], entsize):
        st_name = struct.unpack_from('<I', d, off)[0]
        st_info = d[off+4]
        st_shndx = struct.unpack_from('<H', d, off+6)[0]
        if st_shndx != SHN_XINDEX:
            continue
        name = strd[st_name:strd.find(b'\0', st_name)].decode('latin1')
        typ = st_info & 0xf
        if name.startswith('__UNIQUE_ID_') or (typ == 3 and name == ''):
            struct.pack_into('<H', d, off+6, modinfo)
            fixed += 1
        else:
            bad.append((name, typ, off))
    open(path, 'wb').write(d)
    print(f'symtab_shndx: fixed={fixed} -> .modinfo[{modinfo}] bad={len(bad)}')
    for b in bad[:5]:
        print('  BAD', b)
    return 0 if not bad else 1

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
