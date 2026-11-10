#!/usr/bin/env python3
"""Build DELTA-MAP/FEATURE-DELTA/BACKPORT-MAP/LEDGER from measured data."""
import os, re, json, subprocess
from collections import Counter, defaultdict
V=os.environ.get('KB_OLD') or os.path.expanduser('~/realme-gt3-kernel'); L=os.environ.get('KB_NEW') or os.path.expanduser('~/src/kernel/linux-7.4-rc2')
OUT=os.environ.get('KB_OUT') or os.path.expanduser('~/kernel-delta-out')
os.makedirs(OUT, exist_ok=True)
J=lambda n: json.load(open(os.path.join(OUT,n)))
summ=J('delta-summary.json'); feat=J('feature-delta.json'); line=J('line-delta.json')

def kconfig(root):
    syms={}
    for dp,dn,fn in os.walk(root):
        dn[:]=[d for d in dn if d not in ('.git','__pycache__')]
        for f in fn:
            if not (f=='Kconfig' or f.startswith('Kconfig.')): continue
            rel=os.path.relpath(os.path.join(dp,f),root)
            try: txt=open(os.path.join(dp,f),encoding='utf-8',errors='replace').read()
            except OSError: continue
            for m in re.finditer(r'(?m)^\s*(?:menu)?config\s+([A-Za-z0-9_]+)\b',txt):
                syms[m.group(1)]=rel
    return syms
vk=kconfig(V); lk=kconfig(L)
kadd=sorted(set(lk)-set(vk)); krem=sorted(set(vk)-set(lk))
with open(os.path.join(OUT,'kconfig-added.tsv'),'w') as f:
    for s in kadd: f.write(f'{s}\t{lk[s]}\n')
with open(os.path.join(OUT,'kconfig-removed.tsv'),'w') as f:
    for s in krem: f.write(f'{s}\t{vk[s]}\n')
kadd_top=Counter(lk[s].split('/')[0] if '/' in lk[s] else '(root)' for s in kadd)
krem_top=Counter(vk[s].split('/')[0] if '/' in vk[s] else '(root)' for s in krem)

tops=sorted(summ['by_top'], key=lambda t: -(summ['by_top'][t]['modified']+summ['by_top'][t]['added']+summ['by_top'][t]['removed']))
def lget(t,k): return line.get(t,{}).get(k,0)
def verdict(t):
    if t in ('Documentation','tools','samples','scripts','usr','LICENSES','certs'): return 'non-runtime/tooling'
    if t=='drivers': return 'vendor/generic split required'
    if t=='arch': return 'arm64-specific; feature-gated'
    if t=='rust': return 'new language support; toolchain/ABI'
    if t in ('mm','kernel','fs','net','security','io_uring','include','lib','block','ipc','crypto','sound','virt','init'): return 'runtime target'
    return 'inventory'

# DELTA-MAP.md
lines=[]
lines.append('# Linux 5.10.236 vendor vs Linux 7.4-rc2 — file-to-file delta map\n')
lines.append('**Date:** 2026-11-10  \n**Vendor:** `~/realme-gt3-kernel` (5.10.236, git `0fd25ffc…`)  \n**Latest beta:** Linux **7.4-rc2** (`Makefile` 7.4.0-rc2, tag commit `7475ea8b…`)  \n**Tarball:** `https://git.kernel.org/torvalds/t/linux-7.4-rc2.tar.gz` sha256 `010063d5fc7bb3a5f1fb116cff45abe4977e8a8205b7841773505680c7a014bb` (274,397,238 B)  \n**Method:** kernel.org `releases.json` (HTTP fetch, 200) + `git ls-remote` tag + tarball; per-file size/cmp inventory; `git diff --no-index --numstat` for line stats. Access date 2026-11-10.\n')
lines.append('## Totals (text/source files; build artifacts excluded)\n')
lines.append('| tree | files | lines | bytes |\n|---|---:|---:|---:|\n')
for k,label in (('vendor_loc','vendor 5.10.236'),('latest_loc','7.4-rc2')):
    x=feat[k]; lines.append(f'| {label} | {x["files"]} | {x["lines"]:,} | {x["bytes"]:,} |\n')
lines.append(f'\n**Delta:** latest has **+{feat["latest_loc"]["lines"]-feat["vendor_loc"]["lines"]:,} lines** ({(feat["latest_loc"]["lines"]/feat["vendor_loc"]["lines"]-1)*100:.1f}%).  \n')
lines.append(f'**File-to-file:** union **{summ["files"]["union"]:,}** paths: **added {summ["totals"]["added"]:,}**, **removed {summ["totals"]["removed"]:,}**, **modified {summ["totals"]["modified"]:,}**, **identical {summ["totals"]["identical"]:,}**. Manifest: `delta-manifest.tsv.gz` ({os.path.getsize(os.path.join(OUT,"delta-manifest.tsv.gz")):,} B gz).\n')
lines.append(f'**Line diff (source-filtered):** **+{sum(x.get("added",0) for x in line.values()):,} / -{sum(x.get("deleted",0) for x in line.values()):,}** across **{sum(x.get("files",0) for x in line.values()):,}** changed files.\n')
lines.append('\n## Top-level delta\n')
lines.append('| top | added | removed | modified | identical | line+ | line- | judgment |\n|---|---:|---:|---:|---:|---:|---:|---|\n')
for t in tops:
    s=summ['by_top'][t]
    lines.append(f'| `{t}` | {s["added"]:,} | {s["removed"]:,} | {s["modified"]:,} | {s["identical"]:,} | {lget(t,"added"):,} | {lget(t,"deleted"):,} | {verdict(t)} |\n')
lines.append('\n## Feature deltas (measured)\n')
lines.append(f'- Kconfig symbols: vendor **{feat["kconfig"]["vendor"]:,}** → latest **{feat["kconfig"]["latest"]:,}**; **+{feat["kconfig"]["added"]:,} / -{feat["kconfig"]["removed"]:,}**.\n')
lines.append(f'- New syscalls in latest vs vendor: **{len(feat["syscalls"]["added"])}**: `'+'`, `'.join(feat['syscalls']['added'])+'`.\n')
lines.append('- New source dirs/features (added files): `rust` 710, `io_uring` 87, `mm` 80, `kernel/sched/ext` 18, `security/landlock` + LSM, `fs/smb/server`, `net/tcp_ao`, `mm/damon`.\n')
lines.append('- MGLRU: vendor `mm/vmscan.c` has **0** `lru_gen` references; 7.4-rc2 has **216**; `Documentation/admin-guide/mm/multigen_lru.rst` absent in vendor.\n')
lines.append('\n## Evidence files\n')
lines.append('- `delta-manifest.tsv.gz` — per-path status + sizes (all 111,989 paths).\n- `delta-summary.json` — per-top counts/bytes.\n- `line-delta.json` — per-subsystem line ±.\n- `feature-delta.json` — LOC/Kconfig/syscalls/new files.\n- `kconfig-added.tsv` / `kconfig-removed.tsv` — symbol → file.\n- `inventory.log`, `features.log`, `line-delta.log` — command logs.\n')
open(os.path.join(OUT,'DELTA-MAP.md'),'w').write(''.join(lines))

# FEATURE-DELTA.md
lines=['# Feature delta 5.10.236 → 7.4-rc2 (selected, measured)\n']
lines.append('## Kconfig added by top-level (count)\n| top | +symbols | -symbols |\n|---|---:|---:|\n')
for t,c in kadd_top.most_common(20): lines.append(f'| `{t}` | {c} | {krem_top.get(t,0)} |\n')
lines.append('\n## Kconfig added symbols (first 80)\n')
for s in kadd[:80]: lines.append(f'- `{s}` — `{lk[s]}`\n')
lines.append('\n## New syscalls\n')
for s in feat['syscalls']['added']: lines.append(f'- `{s}`\n')
lines.append('\n## Added files in key dirs (top by size)\n')
for k,v in feat['added_key_dirs'].items():
    lines.append(f'\n### `{k}` — {v["count"]} files, {v["bytes"]:,} B\n')
    for p,sz in v['top'][:12]: lines.append(f'- `{p}` ({sz:,} B)\n')
open(os.path.join(OUT,'FEATURE-DELTA.md'),'w').write(''.join(lines))

# BACKPORT-MAP.md
lines=['# Backport map: 7.4-rc2 functionality → vendor 5.10 runtime\n']
lines.append('**Scope:** CVE fixes excluded. Program: runtime backport of newer-kernel functionality via the proven entry/payload bus.  \n**Entry:** vendor-ABI LKM + aebuilder CFI shim + real `__versions` + SHN_XINDEX fix + `set_memory_x` payload (QEMU-proven, sha `5c015fdb…`).  \n**Rule:** no feature is “backported” until QEMU behavior/metrics + rollback; source presence is not proof.\n')
lines.append('\n## Strategy tiers\n')
lines.append('| tier | scope | mechanism | status |\n|---|---|---|---|\n')
lines.append('| T0 | entry/payload bus/ABI | LKM + CFI shim + symbol registry + `set_memory_x` | **verified in QEMU** |\n')
lines.append('| T1 | mm core (MGLRU, VMA, DAMON, alloc_tag, memcontrol) | kprobe observability → policy hooks → trampolines/text patch; side tables for missing fields | planned |\n')
lines.append('| T2 | kernel/sched, fs, net, security, io_uring, UAPI | feature modules/hooks; syscall table patch only with CFI-safe trampolines | planned |\n')
lines.append('| T3 | arch/arm64, drivers, sound, crypto, block, lib, virt, init | source-level port + module/hook; generic drivers first; vendor-specific excluded from runtime path | inventory |\n')
lines.append('| T4 | Documentation, tools, samples, scripts, usr, LICENSES, certs, android | tooling/docs; not runtime | non-runtime |\n')
lines.append('\n## All top-level trees (no omissions)\n')
lines.append('| top | delta files (+/-/~) | line ± | backport mechanism | priority | status |\n|---|---:|---:|---|---|---|\n')
prio={'mm':'T1','include':'T1','kernel':'T2','fs':'T2','net':'T2','security':'T2','io_uring':'T2','rust':'T2','drivers':'T3','arch':'T3','sound':'T3','crypto':'T3','block':'T3','lib':'T3','virt':'T3','init':'T3','ipc':'T3','Documentation':'T4','tools':'T4','samples':'T4','scripts':'T4','usr':'T4','LICENSES':'T4','certs':'T4','android':'T4','(root)':'T4'}
mech={'mm':'kprobe/trampoline + side tables','kernel':'feature modules/hooks','fs':'VFS/fs feature modules','net':'net feature modules/hooks','security':'LSM/hook modules','io_uring':'new syscall/table + module','rust':'Rust-for-Linux infra; toolchain/ABI','drivers':'generic driver source port; vendor-specific out','arch':'arm64 feature port; CPU-gated','sound':'ALSA source port','crypto':'crypto source port','block':'block layer source port','lib':'library source port','virt':'virt source port','init':'init/Kconfig source port','ipc':'IPC source port','include':'UAPI/header port','Documentation':'docs','tools':'tooling','samples':'samples','scripts':'build scripts','usr':'initramfs/tooling','LICENSES':'licenses','certs':'certs','android':'vendor-only','(root)':'root files'}
for t in tops:
    s=summ['by_top'][t]
    lines.append(f'| `{t}` | +{s["added"]:,}/-{s["removed"]:,}/~{s["modified"]:,} | +{lget(t,"added"):,}/-{lget(t,"deleted"):,} | {mech.get(t,"source port")} | {prio.get(t,"T4")} | inventory |\n')
lines.append('\n## Feature ledger (first wave)\n')
lines.append('| id | feature | 7.4 source | 5.10 target | mechanism | priority | status |\n|---|---|---|---|---|---|---|\n')
features=[
 ('F-001','MGLRU','mm/vmscan.c (lru_gen*, 216 refs), mm/workingset.c, Documentation/.../multigen_lru.rst','mm/vmscan.c, mm/workingset.c, lruvec/page fields','Stage A kprobe observability; Stage B policy hook; Stage C trampolines; side table for generations','T1','planned'),
 ('F-002','VMA rework','mm/vma.c (3,715 L), mm/mmap.c changes','mm/mmap.c, mm/memory.c','source-level port or trampolines; high risk','T1','inventory'),
 ('F-003','DAMON','mm/damon/core.c (4,311 L), sysfs','mm/damon absent','source/module; debugfs/sysfs hooks','T1','inventory'),
 ('F-004','alloc_tag / memory allocation tagging','mm/alloc_tag.c (1,367 L)','mm/','source/module; page_ext?','T1','inventory'),
 ('F-005','memcontrol-v1','mm/memcontrol-v1.c (2,218 L)','mm/memcontrol.c','source port; cgroup ABI','T2','inventory'),
 ('F-006','sched_ext','kernel/sched/ext/ext.c 12,499 L + sub.c 3,229 L','kernel/sched/ absent','module/hooks; BPF scheduler integration','T2','inventory'),
 ('F-007','io_uring new features','io_uring/zcrx.c 1,970 L, net.c 2,013 L','io_uring/ (vendor has 4 files, 4 modified)','syscall/table + module; high risk','T2','inventory'),
 ('F-008','Landlock/LSM','security/landlock/*, security/lsm_init.c','security/ absent','LSM module/hooks; UAPI headers','T2','inventory'),
 ('F-009','TCP-AO / net features','net/ipv4/tcp_ao.c, MPTCP, Bluetooth ISO','net/','source/module hooks','T2','inventory'),
 ('F-010','Rust for Linux','rust/ 710 files, 6.4 MB','absent','toolchain + Rust abstractions; not a drop-in module','T2','inventory'),
 ('F-011','31 new syscalls','include/uapi/asm-generic/unistd.h; syscall tables','arch/arm64 syscall table','syscall table patch + CFI-safe trampolines; UAPI headers','T2','inventory'),
 ('F-012','generic drivers','drivers/ +14,250 files','drivers/','source-level port; vendor-specific excluded; QEMU/device per driver','T3','inventory'),
 ('F-013','arm64 arch features','arch/ +7,767 files','arch/arm64','feature-gated source port; CPU capability checks','T3','inventory'),
]
for f in features: lines.append('| '+' | '.join(f)+' |\n')
lines.append('\n## “Backport everything” statement\nAll top-level trees above are enumerated; root files are the `(root)` row. The source-level delta is complete (manifest + line stats); execution is staged by tier. Non-runtime trees are explicitly classified, not silently omitted. No feature row is marked verified until QEMU metrics/behavior and rollback are recorded.\n')
lines.append('\n## Constraints\n- CVE fixes out of scope.\n- Do not depend on `KEXEC/DEVMEM/MULTIKERNEL/LIVEPATCH/FUNCTION_TRACER` (not set).\n- Kprobes/BPF/modules are available; CFI/BTI/PAC/SCS/STRICT_RWX must be respected.\n- QEMU exact Image first; device only with backup/rollback; no rmmod until unload proven.\n')
open(os.path.join(OUT,'BACKPORT-MAP.md'),'w').write(''.join(lines))

# Ledger TSV
with open(os.path.join(OUT,'BACKPORT-LEDGER.tsv'),'w') as f:
    f.write('id\tscope\tfeature\tsource\ttarget\tmechanism\tpriority\tstatus\tevidence\n')
    for t in tops:
        s=summ['by_top'][t]
        f.write(f'TOP-{t}\t{t}\ttree delta\t5.10.236 vs 7.4-rc2\t{t}\t{mech.get(t,"source port")}\t{prio.get(t,"T4")}\tinventory\tdelta-manifest.tsv.gz / line-delta.json\n')
    for x in features:
        f.write('\t'.join(x)+'\tsource paths in BACKPORT-MAP.md\n')
print('wrote maps; kconfig+',len(kadd),'kconfig-',len(krem),'tops',len(tops))
