#!/usr/bin/env python3
"""Normalize an out-of-tree .modinfo blob for the RMX3709 vendor kernel.

The local build emits entries prefixed with the module name and a vermagic
that does not match the running vendor kernel.  This rewrites the blob to:

  * strip the "<modname>." prefix from every entry,
  * force vermagic / name / depends / license (license only if missing),
  * drop srcversion (build-path leak, unused by the loader),
  * preserve EVERY parm/parmtype entry, including duplicate keys,
  * pad back to the original length so the ELF section size never grows.

Duplicate keys are legitimate: a module with N parameters emits N "parm="
and N "parmtype=" entries.  A dict-based rewrite silently dropped all but
the last one, which is why this keeps an ordered (key, value) list.

Usage: modinfo_fix.py <blob> <modname> <vermagic>
"""
import sys

DROP = {b"srcversion"}


def main():
    path, modname, vermagic = sys.argv[1:4]
    raw = open(path, "rb").read()
    prefix = modname.encode() + b"."
    entries = [e for e in raw.split(b"\0") if e]

    parsed = []  # list of [key, value], duplicates preserved
    for e in entries:
        if e.startswith(prefix):
            e = e[len(prefix):]
        key, sep, val = e.partition(b"=")
        if not sep:
            continue
        if key in DROP:
            continue
        parsed.append([key, val])

    def force(key, val):
        for kv in parsed:
            if kv[0] == key:
                kv[1] = val
                return
        parsed.append([key, val])

    force(b"vermagic", vermagic.encode())
    force(b"name", modname.encode())
    force(b"depends", b"")
    if not any(k == b"license" for k, _ in parsed):
        parsed.append([b"license", b"GPL"])

    out = b"".join(k + b"=" + v + b"\0" for k, v in parsed)
    if len(out) > len(raw):
        print("FAIL modinfo grew %d -> %d bytes" % (len(raw), len(out)))
        return 1
    out = out.ljust(len(raw), b"\0")
    open(path, "wb").write(out)
    print("modinfo ok: %d bytes, %d entries" % (len(out), len(parsed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
