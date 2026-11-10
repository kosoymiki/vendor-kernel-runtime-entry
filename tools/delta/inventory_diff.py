#!/usr/bin/env python3
"""File-to-file inventory/delta: vendor 5.10.236 vs Linux 7.4-rc2."""
import os, sys, gzip, json, subprocess, filecmp
from collections import defaultdict

V = os.environ.get('KB_OLD') or os.path.expanduser('~/realme-gt3-kernel')
L = os.environ.get('KB_NEW') or os.path.expanduser('~/src/kernel/linux-7.4-rc2')
OUT = os.environ.get('KB_OUT') or os.path.expanduser('~/kernel-delta-out')
os.makedirs(OUT, exist_ok=True)

EXCLUDE_DIRS = {'.git', '__pycache__', '.cache'}
EXCLUDE_EXT = ('.o', '.cmd', '.ko', '.mod', '.a', '.so', '.pyc', '.orig', '.rej', '.tmp')
EXCLUDE_NAMES = {'modules.order', 'Module.symvers', '.config', '.config.old'}

def keep(rel):
    parts = rel.split('/')
    if any(p in EXCLUDE_DIRS for p in parts):
        return False
    name = parts[-1]
    if name in EXCLUDE_NAMES:
        return False
    return not name.endswith(EXCLUDE_EXT)

def vendor_files():
    out = subprocess.check_output(['git', '-C', V, 'ls-files', '-z'])
    for rel in out.split(b'\0'):
        if not rel: continue
        rel = rel.decode('utf-8', 'replace')
        if keep(rel): yield rel

def latest_files():
    for root, dirs, files in os.walk(L):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, L)
            if keep(rel): yield rel

def sig(path):
    try:
        st = os.lstat(path)
        if os.path.islink(path):
            return ('link', os.readlink(path))
        return ('file', st.st_size)
    except OSError:
        return ('missing', 0)

def main():
    vs = {r: sig(os.path.join(V, r)) for r in vendor_files()}
    ls = {r: sig(os.path.join(L, r)) for r in latest_files()}
    paths = sorted(set(vs) | set(ls))
    summary = defaultdict(lambda: {'added':0,'removed':0,'modified':0,'identical':0,'vendor_bytes':0,'latest_bytes':0})
    rows = []
    for rel in paths:
        v = vs.get(rel); l = ls.get(rel)
        top = rel.split('/')[0] if '/' in rel else '(root)'
        s = summary[top]
        if v is None:
            st = 'added'; s['added'] += 1; s['latest_bytes'] += l[1] if l[0]=='file' else 0
        elif l is None:
            st = 'removed'; s['removed'] += 1; s['vendor_bytes'] += v[1] if v[0]=='file' else 0
        elif v[0] != l[0]:
            st = 'modified'; s['modified'] += 1
        elif v[0] == 'link':
            st = 'identical' if v[1] == l[1] else 'modified'
            if st == 'modified': s['modified'] += 1
            else: s['identical'] += 1
        else:
            vb, lb = v[1], l[1]
            s['vendor_bytes'] += vb; s['latest_bytes'] += lb
            if vb != lb:
                st = 'modified'; s['modified'] += 1
            else:
                try:
                    same = filecmp.cmp(os.path.join(V, rel), os.path.join(L, rel), shallow=False)
                except OSError:
                    same = False
                st = 'identical' if same else 'modified'
                s['identical' if same else 'modified'] += 1
        rows.append((rel, st, v[1] if v and v[0]=='file' else 0, l[1] if l and l[0]=='file' else 0))
    with gzip.open(os.path.join(OUT, 'delta-manifest.tsv.gz'), 'wt', encoding='utf-8') as f:
        f.write('path\tstatus\tvendor_bytes\tlatest_bytes\n')
        for r in rows: f.write('%s\t%s\t%d\t%d\n' % r)
    with open(os.path.join(OUT, 'delta-summary.json'), 'w', encoding='utf-8') as f:
        json.dump({'vendor': V, 'latest': L, 'files': {'vendor': len(vs), 'latest': len(ls), 'union': len(paths)},
                   'by_top': dict(sorted(summary.items(), key=lambda kv: -(kv[1]['modified']+kv[1]['added']+kv[1]['removed']))),
                   'totals': {k: sum(x[k] for x in summary.values()) for k in ('added','removed','modified','identical','vendor_bytes','latest_bytes')}},
                  f, indent=2, ensure_ascii=False)
    print(json.dumps({'files': len(paths), 'totals': {k: sum(x[k] for x in summary.values()) for k in ('added','removed','modified','identical')}}, ensure_ascii=False))
    for top, s in sorted(summary.items(), key=lambda kv: -(kv[1]['modified']+kv[1]['added']+kv[1]['removed']))[:25]:
        print(f"{top}\t+{s['added']} -{s['removed']} ~{s['modified']} ={s['identical']}")

if __name__ == '__main__':
    main()
