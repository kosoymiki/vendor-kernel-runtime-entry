#!/usr/bin/env python3
"""LOC, Kconfig, syscall and new-file feature delta for 5.10.236 vs 7.4-rc2."""
import os, re, json, gzip, subprocess
from collections import Counter, defaultdict
V=os.environ.get('KB_OLD') or os.path.expanduser('~/realme-gt3-kernel'); L=os.environ.get('KB_NEW') or os.path.expanduser('~/src/kernel/linux-7.4-rc2')
OUT=os.environ.get('KB_OUT') or os.path.expanduser('~/kernel-delta-out')
os.makedirs(OUT, exist_ok=True)

def git_files(root):
    out=subprocess.check_output(['git','-C',root,'ls-files','-z'])
    return [x.decode('utf-8','replace') for x in out.split(b'\0') if x]

def latest_files(root):
    for dp,dn,fn in os.walk(root):
        dn[:]=[d for d in dn if d not in ('.git','__pycache__')]
        for f in fn: yield os.path.relpath(os.path.join(dp,f),root)

def is_text(path):
    try:
        with open(path,'rb') as f: b=f.read(8192)
        return b'\0' not in b
    except OSError: return False

def loc(root, files):
    lines=0; files_n=0; bytes_n=0
    for rel in files:
        p=os.path.join(root,rel)
        try:
            sz=os.path.getsize(p)
            if not is_text(p): continue
            with open(p,'rb') as f:
                data=f.read()
            lines += data.count(b'\n') + (0 if data.endswith(b'\n') else 1)
            files_n += 1; bytes_n += sz
        except OSError: pass
    return {'files':files_n,'lines':lines,'bytes':bytes_n}

def kconfig(root, files):
    syms={}
    for rel in files:
        if not (rel.endswith('Kconfig') or os.path.basename(rel).startswith('Kconfig.')): continue
        try: text=open(os.path.join(root,rel),encoding='utf-8',errors='replace').read()
        except OSError: continue
        for m in re.finditer(r'(?m)^\s*(?:menu)?config\s+([A-Za-z0-9_]+)\b',text):
            syms[m.group(1)]=rel
    return syms

def syscalls(root):
    res={}
    for rel in ('arch/arm64/kernel/syscall.tbl','include/uapi/asm-generic/unistd.h'):
        p=os.path.join(root,rel)
        if not os.path.exists(p): continue
        for line in open(p,encoding='utf-8',errors='replace'):
            if 'syscall.tbl' in rel:
                if line.startswith('#'): continue
                parts=line.split()
                if len(parts)>=4 and parts[0].isdigit():
                    res[parts[3]]=rel
            else:
                m=re.search(r'__NR(?:3264)?_([A-Za-z0-9_]+)\s+(\d+)',line)
                if m: res[m.group(1)]=rel
    return res

def main():
    vf=git_files(V); lf=list(latest_files(L))
    res={'vendor_loc':loc(V,vf),'latest_loc':loc(L,lf)}
    vk=kconfig(V,vf); lk=kconfig(L,lf)
    res['kconfig']={'vendor':len(vk),'latest':len(lk),'added':len(set(lk)-set(vk)),'removed':len(set(vk)-set(lk))}
    vs=syscalls(V); ls=syscalls(L)
    res['syscalls']={'vendor':len(vs),'latest':len(ls),'added':sorted(set(ls)-set(vs)),'removed':sorted(set(vs)-set(ls))}
    # added files by top dir from manifest
    added=defaultdict(list)
    with gzip.open(os.path.join(OUT,'delta-manifest.tsv.gz'),'rt',encoding='utf-8') as f:
        next(f)
        for line in f:
            path,status,vb,lb=line.rstrip('\n').split('\t')
            if status=='added':
                added[path.split('/')[0] if '/' in path else '(root)'].append((path,int(lb)))
    res['added_files_top']={k:{'count':len(v),'bytes':sum(x[1] for x in v)} for k,v in added.items()}
    res['added_key_dirs']={}
    for key in ('rust','io_uring','mm','kernel/sched','fs','net','security','include/uapi/linux'):
        items=[x for x in added.get(key.split('/')[0],[]) if x[0].startswith(key+'/')]
        res['added_key_dirs'][key]={'count':len(items),'bytes':sum(x[1] for x in items),'top':sorted(items,key=lambda x:-x[1])[:15]}
    json.dump(res,open(os.path.join(OUT,'feature-delta.json'),'w'),indent=2,ensure_ascii=False)
    print(json.dumps({k:res[k] for k in ('vendor_loc','latest_loc','kconfig','syscalls')},indent=2,ensure_ascii=False)[:4000])

if __name__=='__main__': main()
