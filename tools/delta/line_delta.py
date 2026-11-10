#!/usr/bin/env python3
"""Per-subsystem line diff (git diff --no-index --numstat), source files only."""
import os, json, subprocess, sys
V=os.environ.get('KB_OLD') or os.path.expanduser('~/realme-gt3-kernel'); L=os.environ.get('KB_NEW') or os.path.expanduser('~/src/kernel/linux-7.4-rc2')
OUT=os.environ.get('KB_OUT') or os.path.expanduser('~/kernel-delta-out')
os.makedirs(OUT, exist_ok=True)
DIRS=['arch','block','certs','crypto','drivers','fs','include','init','ipc','io_uring','kernel','lib','mm','net','rust','samples','scripts','security','sound','tools','usr','virt','Documentation','LICENSES']
BAD=('.o','.cmd','.ko','.mod','.a','.so','.pyc','.orig','.rej','.tmp','.git/','__pycache__')
def good(p):
    p=p.replace('\\','/')
    return not any(x in p for x in BAD)
res={}
rows=[]
for d in DIRS:
    a=os.path.join(V,d); b=os.path.join(L,d)
    if not (os.path.isdir(a) and os.path.isdir(b)): continue
    cmd=['git','diff','--no-index','--numstat',a,b]
    try:
        p=subprocess.run(cmd,capture_output=True,text=True,timeout=1500)
    except subprocess.TimeoutExpired:
        res[d]={'error':'timeout'}; print(d,'TIMEOUT',flush=True); continue
    add=dele=files=0
    for line in p.stdout.splitlines():
        parts=line.split('\t')
        if len(parts)<3: continue
        if not good(parts[2]): continue
        try:
            x=int(parts[0]); y=int(parts[1])
        except ValueError:
            continue
        add+=x; dele+=y; files+=1
        rows.append((d,parts[2],x,y))
    res[d]={'added':add,'deleted':dele,'files':files,'rc':p.returncode}
    print(d,res[d],flush=True)
json.dump(res,open(os.path.join(OUT,'line-delta.json'),'w'),indent=2)
import gzip
with gzip.open(os.path.join(OUT,'file-line-delta.tsv.gz'),'wt',encoding='utf-8') as f:
    f.write('top\tpath\tadded\tdeleted\n')
    for r in rows: f.write('%s\t%s\t%d\t%d\n' % r)
print('TOTAL', {'added':sum(x.get('added',0) for x in res.values()),'deleted':sum(x.get('deleted',0) for x in res.values()),'files':sum(x.get('files',0) for x in res.values())})
