// ktramp: replace get_scan_count with a module wrapper via a copied-prologue
// trampoline. Proves full-function text replacement (not only return-path).
// QEMU 1 CPU uses aarch64_insn_patch_text_nosync; SMP needs stop_machine.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/stddef.h>
#include <linux/tracepoint.h>
#include "ktramp_policy.h"

void *memset(void *s,int c,size_t n){unsigned char*p=s;while(n--)*p++=(unsigned char)c;return s;}
typedef unsigned long (*kallsyms_fn)(const char*);
typedef void *(*alloc_fn)(unsigned long);
typedef int (*setx_fn)(unsigned long,int);
typedef void (*flush_fn)(unsigned long,unsigned long);
typedef int (*patch_fn)(void*,u32);
typedef int (*mpatch_fn)(void**,u32*,int);
typedef struct proc_dir_entry *(*pc_fn)(const char*,umode_t,struct proc_dir_entry*,const struct proc_ops*);
typedef void (*pr_fn)(struct proc_dir_entry*);
typedef long (*srb_fn)(void __user*,size_t,loff_t*,const void*,size_t);
typedef long (*swb_fn)(void*,size_t,loff_t*,const void __user*,size_t);
typedef int (*tpreg_fn)(struct tracepoint*,void*,void*);
typedef int (*tpunreg_fn)(struct tracepoint*,void*,void*);
typedef struct lruvec *(*ptl_fn)(struct page*, void*);

static unsigned long target;          /* get_scan_count */
static u32 orig0;
static void *tramp;
static void (*tramp_fn)(void*,void*,void*);
static unsigned long calls, refaults, patched, last_dist, dist_sum, sum_nr, nr_calls, policy_applied;
static int policy_on;
static unsigned long cur_gen = 1, gen_tick;
#define LVN 256
static unsigned long lvkey[LVN], lvacc[LVN], lvgen[LVN], lvref[LVN], lvdist[LVN];
static unsigned long lvyoung[LVN], lvcold[LVN], accesses, y20, c20, y40, c40;
static int cur_lv = -1;
#define EVN 8192
static unsigned long epage[EVN], egen[EVN];
static struct kprobe ekp = { .symbol_name = "workingset_eviction" };
static struct kprobe akp = { .symbol_name = "mark_page_accessed" };
static int ak_reg;
#define MGN 256
static unsigned long mgkey[MGN], mggen[MGN], mgref[MGN], mgscan[MGN];
static unsigned long mgseq[MGN], mgwin[MGN];   /* per-memcg scan-epoch clock */
static unsigned long mgev[MGN], mgttl[MGN], ev_age[MGN][4]; /* thrash + eviction age */
static unsigned long last_seq, last_win, last_ttl;
static int last_mgi = -1;
static unsigned long mgattr_exact, mgattr_fb, folio_heads, dist_hist[4], branch_hist[10];
static unsigned long snap_sum, snap_pol, snap_sum2, snap_pol2; /* fixed endpoints: calls 1..20 and 1..40 */
static void *cur_memcg;
static unsigned long tp_seq, last_tp_seq, last_kref;
static long *nr_swap_pages_p;
static ptl_fn page_to_lruvec_fn;
static void *contig_pgdat;
static unsigned long sum_anon, sum_file, sa20, sf20, sa40, sf40;
static unsigned long pre_total, pol_delta, pre20, d20, pre40, d40;
static unsigned long force_d;   /* fault injection: force refault distance */
static unsigned long force_young, force_cold; /* fault injection: force young/cold split */
static unsigned long force_ao, force_aa;     /* fault injection: force age split */
static unsigned long ev_hits, last_reuse_dist, last_ev, last_nonres, last_true_dist;
static unsigned long lvmemcg[LVN];
static tpreg_fn tp_reg; static tpunreg_fn tp_unreg; static void *tp_ptr; static int tp_attached;
static mpatch_fn mpatch;
static patch_fn do_patch;
static pr_fn k_remove; static srb_fn k_srb; static swb_fn k_swb;
static struct proc_dir_entry *kpde;
static char kbuf[512]; static char *kp=kbuf,*kend=kbuf+sizeof(kbuf)-1;
static struct kprobe rkp = { .symbol_name = "workingset_refault" };

static void putc(char c){if(kp<kend)*kp++=c;*kp=0;}
static void put(const char*s){while(*s)putc(*s++);}
static void puthex(unsigned long v){int sh,st=0;put("0x");for(sh=(int)(sizeof(unsigned long)*8)-4;sh>=0;sh-=4){unsigned n=(unsigned)((v>>sh)&0xf);char c=n<10?(char)('0'+n):(char)('a'+n-10);if(!st&&c=='0'&&sh)continue;st=1;putc(c);}}
static void putdec(long v){unsigned long u=v<0?(unsigned long)-v:(unsigned long)v,d=1;if(v<0)putc('-');while(u/d>=10)d*=10;while(d){putc((char)('0'+(u/d)%10));d/=10;}}

static int lv_touch(void *lv)
{
	unsigned long key=(unsigned long)lv, idx=(key>>4)&(LVN-1), i;
	for(i=0;i<LVN;i++){unsigned long j=(idx+i)&(LVN-1);
		if(*(lvkey+j)==key||*(lvkey+j)==0){
			*(lvkey+j)=key; (*(lvacc+j))++;
			if((*(lvacc+j)&63)==0){ (*(lvgen+j))++; cur_gen++; }
			return (int)j;
		}}
	return -1;
}

static void ev_store(unsigned long page, unsigned long gen)
{
	kt_ev_store(epage, egen, EVN, page, gen);
}
static unsigned long ev_load(unsigned long page)
{
	return kt_ev_load(epage, egen, EVN, page);
}

/* Folio registry: compound tails share the head page's generation. */
static unsigned long folio_head(unsigned long page)
{
	return kt_folio_head(page, &folio_heads);
}

/* Per-memcg generation row (mem_cgroup pointer identity). */
static int lv_find(void *lv)
{
	unsigned long key=(unsigned long)lv, idx=(key>>4)&(LVN-1), i;
	if(!key) return -1;
	for(i=0;i<LVN;i++){unsigned long j=(idx+i)&(LVN-1);
		if(*(lvkey+j)==key||*(lvkey+j)==0){
			*(lvkey+j)=key;
			return (int)j;
		}}
	return -1;
}

static int mg_find(unsigned long memcg)
{
	unsigned long idx=(memcg>>4)&(MGN-1), i;
	if(!memcg) return -1;
	for(i=0;i<MGN;i++){unsigned long j=(idx+i)&(MGN-1);
		if(*(mgkey+j)==memcg||*(mgkey+j)==0){
			*(mgkey+j)=memcg;
			return (int)j;
		}}
	return -1;
}

static int mg_touch(unsigned long memcg, int ref)
{
	unsigned long idx=(memcg>>4)&(MGN-1), i;
	if(!memcg) return -1;
	for(i=0;i<MGN;i++){unsigned long j=(idx+i)&(MGN-1);
		if(*(mgkey+j)==memcg||*(mgkey+j)==0){
			*(mgkey+j)=memcg;
			if(ref)(*(mgref+j))++;
			return (int)j;
		}}
	return -1;
}

static int lv_for_memcg(unsigned long memcg);

static int a_handler(struct kprobe *p, struct pt_regs *r)
{
	unsigned long page=r->regs[0];
	int mi;
	(void)p;
	if(!page) return 0;
	page=folio_head(page);
	{
		unsigned long memcg=*(unsigned long *)(page+offsetof(struct page, mem_cgroup));
		mi=mg_find(memcg);
		ev_store(page, (mi>=0 && *(mgseq+mi)) ? *(mgseq+mi) : (cur_gen ? cur_gen : 1));
	}
	if(page_to_lruvec_fn && contig_pgdat){
		struct lruvec *plv=page_to_lruvec_fn((struct page *)page, contig_pgdat);
		if(plv){ mi=lv_find(plv); if(mi>=0) (*(lvacc+mi))++; }
	}
	accesses++;
	return 0;
}

static int ev_handler(struct kprobe *p, struct pt_regs *r)
{
	unsigned long page=r->regs[0], memcg, eg, cur;
	int mi, mgi;
	(void)p;
	if(page){ page=folio_head(page);
		eg=ev_load(page);
		memcg=*(unsigned long *)(page+offsetof(struct page, mem_cgroup));
		mg_touch(memcg,0);
		mgi=mg_find(memcg);
		cur=(mgi>=0 && *(mgseq+mgi)) ? *(mgseq+mgi) : cur_gen;
		if(mgi>=0){
			unsigned long a = (eg && cur) ? (cur - eg) : 99;
			int b = a==0 ? 0 : a==1 ? 1 : a==2 ? 2 : 3;
			(*(mgev+mgi))++;
			(*(ev_age[mgi]+b))++;
		}
		mi=lv_for_memcg(memcg);
		if(mi<0) mi=cur_lv;
		if(mi>=0){
			if(eg && cur && eg==cur) (*(lvyoung+mi))++;
			else (*(lvcold+mi))++;
		}
	}
	return 0;
}

static void wrap(void *lv, void *sc, void *nr)
{
	int exact;
	int mi = -1;
	unsigned long memcg = 0;

	calls++;
	cur_lv = lv_touch(lv);
	exact = (tp_seq != last_tp_seq);
	if (exact) {
		last_tp_seq = tp_seq;
		memcg = (unsigned long)cur_memcg;
		if (cur_lv >= 0 && memcg)
			*(lvmemcg + cur_lv) = memcg;
	}
	tramp_fn(lv, sc, nr);
	if (policy_on) {
		unsigned long *n = nr;
		unsigned long pre = n[0] + n[1] + n[2] + n[3];
		unsigned long kref = (cur_lv >= 0) ? *(volatile unsigned long *)((char *)lv + 112) : 0;
		unsigned long d = force_d ? force_d : ((cur_lv >= 0) ? *(lvdist + cur_lv) : 0);
		int gen = 0;

		last_kref = kref;
		if (exact && memcg) {
			mi = mg_find(memcg);
			if (mi >= 0) {
				last_seq = *(mgseq + mi);
				last_win = *(mgwin + mi) ? *(mgwin + mi) : 1;
				last_ttl = *(mgttl + mi) ? *(mgttl + mi) : 2;
				last_mgi = mi;
				gen = (int)((last_seq ? last_seq : *(mggen + mi)) & 1);
				if ((++(*(mgscan + mi)) & 63) == 0)
					(*(mggen + mi))++;
				mgattr_exact++;
			}
		} else {
			gen = (cur_lv >= 0) ? (int)(*(lvgen + cur_lv) & 1) : 0;
			mgattr_fb++;
		}
		{
			int branch = 0;
			int can_swap = nr_swap_pages_p ? (*nr_swap_pages_p > 0) : 0;
			unsigned long y = (cur_lv >= 0) ? *(lvyoung + cur_lv) : 0;
			unsigned long c = (cur_lv >= 0) ? *(lvcold + cur_lv) : 0;
			unsigned long ao = 0, aa = 0;
			if (force_young || force_cold) { y = force_young; c = force_cold; }
			if (mi >= 0) {
				ao = *(ev_age[mi]+2) + *(ev_age[mi]+3);
				aa = *(ev_age[mi]+0) + *(ev_age[mi]+1) + ao;
			}
			if (force_aa) { ao = force_ao; aa = force_aa; }
			kt_policy_nr(n, d, 32UL * last_ttl, gen, can_swap, y, c, ao, aa, &branch);
			if (branch > 0 && branch < 10)
				branch_hist[branch]++;
		}
		pre_total += pre;
		pol_delta += pre - (n[0] + n[1] + n[2] + n[3]);
		policy_applied++;
	}
	{
		unsigned long *n = nr;
		sum_nr += n[0] + n[1] + n[2] + n[3];
		sum_anon += n[0] + n[1];
		sum_file += n[2] + n[3];
		nr_calls++;
		if (nr_calls == 20) { snap_sum = sum_nr; snap_pol = policy_applied;
			sa20 = sum_anon; sf20 = sum_file;
			pre20 = pre_total; d20 = pol_delta;
			y20 = (cur_lv >= 0) ? *(lvyoung + cur_lv) : 0;
			c20 = (cur_lv >= 0) ? *(lvcold + cur_lv) : 0; }
		if (nr_calls == 40) { snap_sum2 = sum_nr; snap_pol2 = policy_applied;
			sa40 = sum_anon; sf40 = sum_file;
			pre40 = pre_total; d40 = pol_delta;
			y40 = (cur_lv >= 0) ? *(lvyoung + cur_lv) : 0;
			c40 = (cur_lv >= 0) ? *(lvcold + cur_lv) : 0; }
	}
}

static int lv_for_memcg(unsigned long memcg)
{
	int i;
	if(!memcg) return -1;
	for(i=0;i<LVN;i++) if(*(lvmemcg+i)==memcg && *(lvkey+i)) return i;
	return -1;
}

static int rk_handler(struct kprobe *p, struct pt_regs *r)
{
	unsigned long page=r->regs[0], shadow=r->regs[1], eg, memcg, ev, nonres, d=0;
	int mi;
	(void)p;
	if(page){
		page=folio_head(page);
		memcg=*(unsigned long *)(page+offsetof(struct page, mem_cgroup));
		eg=ev_load(page);
		if(eg){ ev_hits++; last_reuse_dist=cur_gen-eg; }
		mg_touch(memcg,1);
		/* Kernel shadow timestamp (mm/workingset.c unpack_shadow):
		 * xa_to_value = shadow >> 2, workingset bit = >> 1,
		 * NODES_SHIFT=0, MEM_CGROUP_ID_SHIFT=16, bucket_order=0
		 * => eviction = shadow >> 19. */
		ev = kt_shadow_eviction(shadow);
		mi = -1;
		if(page_to_lruvec_fn && contig_pgdat){
			struct lruvec *plv = page_to_lruvec_fn((struct page *)page, contig_pgdat);
			if(plv){
				mi = lv_find(plv);
				if(mi>=0)
					nonres = *(volatile unsigned long *)((char *)plv + 96);
			}
		} else {
			mi = lv_for_memcg(memcg);
			if(mi>=0)
				nonres = *(volatile unsigned long *)(*(lvkey+mi) + 96);
		}
		if(mi>=0){
			if(nonres >= ev) d = nonres - ev;
			*(lvdist+mi) = d;
			last_dist = d;
			last_ev = ev; last_nonres = nonres; last_true_dist = d;
			dist_hist[kt_dist_bucket(d)]++;
		}
	}
	refaults++;
	if(cur_lv>=0)(*(lvref+cur_lv))++;
	return 0;
}

static u32 br(u32 insn_addr, unsigned long dest, unsigned long src)
{
	long off=(long)(dest-src);
	(void)insn_addr;
	if(off>=(128L<<20)||off<-(128L<<20)) return 0;
	return 0x14000000u | ((u32)(off>>2)&0x03ffffffu);
}

static void tp_memcg_probe(void *data, void *memcg, bool *skip)
{
	int mi;
	(void)data; (void)skip;
	cur_memcg = memcg;
	tp_seq++;
	mi = mg_find((unsigned long)memcg);
	if (mi >= 0) {
		(*(mgseq + mi))++;
		(*(mgwin + mi)) = (*(mgseq + mi)) > 2 ? 2 : (*(mgseq + mi));
		*(mgttl + mi) = kt_ttl_step(*(mgev + mi), *(mgref + mi),
					    *(mgttl + mi));
	}
}

static void tp_attach(void)
{
	if (tp_attached || !tp_ptr || !tp_reg) return;
	if (tp_reg((struct tracepoint *)tp_ptr, (void *)tp_memcg_probe, (void *)0) == 0) {
		tp_attached = 1;
		pr_info("ktramp: memcg tracepoint attached\n");
	} else {
		pr_warn("ktramp: memcg tracepoint attach failed\n");
	}
}

static void tp_detach(void)
{
	if (!tp_attached || !tp_ptr || !tp_unreg) return;
	tp_unreg((struct tracepoint *)tp_ptr, (void *)tp_memcg_probe, (void *)0);
	tp_attached = 0;
	cur_memcg = (void *)0;
}

static int patch_on(void)
{
	u32 *tp;
	int i;
	if(patched) return 0;
	if(!target||!do_patch) return -EINVAL;
	orig0=*(volatile u32*)target;
	tramp=(void*)((alloc_fn)0);
	/* allocate trampoline */
	{
		alloc_fn a=(alloc_fn)0; setx_fn sx=(setx_fn)0; flush_fn fl=(flush_fn)0;
		/* resolved addresses stored in globals below */
		extern unsigned long kln_sym(const char*);
		a=(alloc_fn)kln_sym("module_alloc");
		sx=(setx_fn)kln_sym("set_memory_x");
		fl=(flush_fn)kln_sym("__flush_icache_range");
		if(!a||!sx||!fl) return -ENOENT;
		tramp=(void*)a(PAGE_SIZE);
		if(!tramp) return -ENOMEM;
		tp=(u32*)tramp;
		for(i=0;i<7;i++) tp[i]=*(volatile u32*)(target+i*4);
		tp[7]=br(0,(unsigned long)(target+28),(unsigned long)(tramp)+28);
		if(!tp[7]) return -ERANGE;
		fl((unsigned long)tramp,(unsigned long)tramp+32);
		sx((unsigned long)tramp,1);
		fl((unsigned long)tramp,(unsigned long)tramp+32);
	}
	tramp_fn=(void(*)(void*,void*,void*))tramp;
	{
		u32 b=br(0,(unsigned long)wrap,(unsigned long)target);
		if(!b) return -ERANGE;
		if (mpatch) {
			void *a[1]; u32 ins[1];
			a[0]=(void*)target; ins[0]=b;
			if (mpatch(a,ins,1)) return -EIO;
		} else if (do_patch((void*)target,b)) return -EIO;
	}
	patched=1;
	return 0;
}

static int patch_off(void)
{
	if(!patched) return 0;
	if (mpatch) { void *a[1]; u32 ins[1]; a[0]=(void*)target; ins[0]=orig0; if(mpatch(a,ins,1)) return -EIO; }
	else if(do_patch((void*)target,orig0)) return -EIO;
	patched=0;
	return 0;
}

static ssize_t k_read(struct file*f,char __user*buf,size_t len,loff_t*off)
{
	(void)f; kp=kbuf;
	put("ktramp target=");puthex(target);put(" tramp=");puthex((unsigned long)tramp);
	put(" patched=");putdec(patched);put(" policy=");putdec(policy_on);put(" calls=");putdec(calls);
	put(" refaults=");putdec(refaults);put(" dist=");putdec(last_dist);put(" sum_nr=");putdec((long)sum_nr);put(" nr_calls=");putdec((long)nr_calls);put(" policy_applied=");putdec((long)policy_applied);put(" sum20=");putdec((long)snap_sum);put(" pol20=");putdec((long)snap_pol);put(" kref=");putdec((long)last_kref);put(" sum40=");putdec((long)snap_sum2);put(" pol40=");putdec((long)snap_pol2);
	{
		int n=0,i; for(i=0;i<LVN;i++) if(*(lvkey+i)) n++;
		put(" lv=");putdec(n);
		n=0; for(i=0;i<MGN;i++) if(*(mgkey+i)) n++;
		put(" mg=");putdec(n);
		put(" mgattr=");putdec((long)mgattr_exact);put("/");putdec((long)mgattr_fb);
		put(" seq=");putdec((long)last_seq);put(" win=");putdec((long)last_win);put(" ttl=");putdec((long)last_ttl);
		{int ai;put(" age=");for(ai=0;ai<4;ai++){if(ai)put(",");putdec((long)(last_mgi>=0?*(ev_age[last_mgi]+ai):0));}}
		put(" folio=");putdec((long)folio_heads);
		put(" dist_hist=");putdec((long)dist_hist[0]);put(",");putdec((long)dist_hist[1]);put(",");putdec((long)dist_hist[2]);put(",");putdec((long)dist_hist[3]);
		put(" branch=");putdec((long)branch_hist[1]);put(",");putdec((long)branch_hist[2]);put(",");putdec((long)branch_hist[3]);put(",");putdec((long)branch_hist[4]);put(",");putdec((long)branch_hist[5]);put(",");putdec((long)branch_hist[6]);put(",");putdec((long)branch_hist[7]);put(",");putdec((long)branch_hist[8]);put(",");putdec((long)branch_hist[9]);
		put(" acc=");putdec((long)accesses);put(" yc=");putdec((long)y20);put("/");putdec((long)c20);put(" yc40=");putdec((long)y40);put("/");putdec((long)c40);
		put(" af20=");putdec((long)sa20);put("/");putdec((long)sf20);put(" af40=");putdec((long)sa40);put("/");putdec((long)sf40);put(" swap=");putdec(nr_swap_pages_p?(*nr_swap_pages_p):-1);
		put(" pre20=");putdec((long)pre20);put(" delta20=");putdec((long)d20);put(" pre40=");putdec((long)pre40);put(" delta40=");putdec((long)d40);
		put(" fd=");putdec((long)force_d);put(" fyc=");putdec((long)force_young);put("/");putdec((long)force_cold);put(" fa=");putdec((long)force_ao);put("/");putdec((long)force_aa);put(" ev=");putdec((long)last_ev);put(" nonres=");putdec((long)last_nonres);put(" tdist=");putdec((long)last_true_dist);put(" evhits=");putdec((long)ev_hits);put(" reuse=");putdec((long)last_reuse_dist);
	}
	put("\n");
	put("off: page.flags=");putdec((long)offsetof(struct page, flags));
	put(" page.mem_cgroup=");putdec((long)offsetof(struct page, mem_cgroup));
	put(" lruvec.size=");putdec((long)sizeof(struct lruvec));
	put(" lruvec.flags=");putdec((long)offsetof(struct lruvec, flags));
	put(" lruvec.pgdat=");putdec((long)offsetof(struct lruvec, pgdat));put("\n");
	if(!k_srb)return -EIO;
	return k_srb(buf,len,off,kbuf,(size_t)(kp-kbuf));
}

static ssize_t k_write(struct file*f,const char __user*ubuf,size_t len,loff_t*ppos)
{
	char tmp[8];long n;int rc;(void)f;
	if(!k_swb)return -EIO;
	n=k_swb(tmp,sizeof(tmp)-1,ppos,ubuf,len);if(n<0)return n;tmp[n]=0;
	if (tmp[0] == 'a') { char *q=tmp+1; force_ao=0; while(*q>='0'&&*q<='9') force_ao=force_ao*10+(unsigned long)(*q++-'0'); if(force_ao>100) force_ao=100; force_aa=100; force_young=40; force_cold=60; policy_on=1; rc=patch_on(); if(!rc) tp_attach(); }
	else if (tmp[0] == 'y') { char *q=tmp+1; force_young=0; while(*q>='0'&&*q<='9') force_young=force_young*10+(unsigned long)(*q++-'0'); if(force_young>100) force_young=100; force_cold=100-force_young; policy_on=1; rc=patch_on(); if(!rc) tp_attach(); }
	else if (tmp[0] == 't') { char *q=tmp+1; force_d=0; while(*q>='0'&&*q<='9') force_d=force_d*10+(unsigned long)(*q++-'0'); policy_on=1; rc=patch_on(); if(!rc) tp_attach(); }
	else if (tmp[0] == '2') { policy_on = 1; rc = patch_on(); if (!rc) tp_attach(); }
	else if (tmp[0] == '1') { tp_detach(); policy_on = 0; rc = patch_on(); }
	else { tp_detach(); policy_on = 0; rc = patch_off(); }
	return rc?rc:n;
}

static const struct proc_ops k_pops={.proc_read=k_read,.proc_write=k_write};

/* global resolver filled in init */
static kallsyms_fn g_kln;
unsigned long kln_sym(const char*name){return g_kln?g_kln(name):0;}

static unsigned long sym(const char*name){struct kprobe k={.symbol_name=name};unsigned long a=0;if(register_kprobe(&k)==0){a=(unsigned long)k.addr;unregister_kprobe(&k);}return a;}

static int __init kt_init(void)
{
	g_kln=(kallsyms_fn)sym("kallsyms_lookup_name");
	if(!g_kln){pr_err("ktramp: no kln\n");return -ENOENT;}
	target=g_kln("get_scan_count");
	do_patch=(patch_fn)g_kln("aarch64_insn_patch_text_nosync");
	mpatch=(mpatch_fn)g_kln("aarch64_insn_patch_text");
	{
		pc_fn pc=(pc_fn)g_kln("proc_create");
		k_remove=(pr_fn)g_kln("proc_remove");
		k_srb=(srb_fn)g_kln("simple_read_from_buffer");
		k_swb=(swb_fn)g_kln("simple_write_to_buffer");
		if(pc&&k_srb)kpde=pc("ktramp",0644,NULL,&k_pops);
	}
	nr_swap_pages_p=(long*)g_kln("nr_swap_pages");
	page_to_lruvec_fn=(ptl_fn)g_kln("page_to_lruvec");
	contig_pgdat=(void*)g_kln("contig_page_data");
	pr_info("ktramp: ptl=%px pgdat=%px\n",(void*)page_to_lruvec_fn,contig_pgdat);
	tp_ptr=(void*)g_kln("__tracepoint_android_vh_shrink_node_memcgs");
	tp_reg=(tpreg_fn)g_kln("tracepoint_probe_register");
	tp_unreg=(tpunreg_fn)g_kln("tracepoint_probe_unregister");
	pr_info("ktramp: tp=%px reg=%px unreg=%px\n",tp_ptr,(void*)tp_reg,(void*)tp_unreg);
	akp.pre_handler=a_handler;
	if(register_kprobe(&akp)==0) ak_reg=1;
	else pr_err("ktramp: access kprobe failed\n");
	ekp.pre_handler=ev_handler;
	if(register_kprobe(&ekp)!=0)pr_err("ktramp: eviction kprobe failed\n");
	rkp.pre_handler=rk_handler;
	if(register_kprobe(&rkp)!=0)pr_err("ktramp: refault kprobe failed\n");
	pr_err("ktramp: target=%px patch=%px proc=%d\n",(void*)target,do_patch,kpde!=NULL);
	return 0;
}

static void __exit kt_exit(void)
{
	tp_detach();
	patch_off();
	if(ak_reg)unregister_kprobe(&akp);
	if(ekp.pre_handler)unregister_kprobe(&ekp);
	if(rkp.pre_handler)unregister_kprobe(&rkp);
	if(kpde&&k_remove)k_remove(kpde);
	pr_err("ktramp: exit\n");
}
module_init(kt_init);module_exit(kt_exit);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("get_scan_count trampoline probe");MODULE_AUTHOR("h2");
