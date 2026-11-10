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

void *memset(void *s,int c,size_t n){unsigned char*p=s;while(n--)*p++=(unsigned char)c;return s;}
typedef unsigned long (*kallsyms_fn)(const char*);
typedef void *(*alloc_fn)(unsigned long);
typedef int (*setx_fn)(unsigned long,int);
typedef void (*flush_fn)(unsigned long,unsigned long);
typedef int (*patch_fn)(void*,u32);
typedef struct proc_dir_entry *(*pc_fn)(const char*,umode_t,struct proc_dir_entry*,const struct proc_ops*);
typedef void (*pr_fn)(struct proc_dir_entry*);
typedef long (*srb_fn)(void __user*,size_t,loff_t*,const void*,size_t);
typedef long (*swb_fn)(void*,size_t,loff_t*,const void __user*,size_t);

static unsigned long target;          /* get_scan_count */
static u32 orig0;
static void *tramp;
static void (*tramp_fn)(void*,void*,void*);
static unsigned long calls, refaults, patched;
static int policy_on;
static unsigned long lvkey[256]; static unsigned long lvcount[256];
static patch_fn do_patch;
static pr_fn k_remove; static srb_fn k_srb; static swb_fn k_swb;
static struct proc_dir_entry *kpde;
static char kbuf[512]; static char *kp=kbuf,*kend=kbuf+sizeof(kbuf)-1;
static struct kprobe rkp = { .symbol_name = "workingset_refault" };

static void putc(char c){if(kp<kend)*kp++=c;*kp=0;}
static void put(const char*s){while(*s)putc(*s++);}
static void puthex(unsigned long v){int sh,st=0;put("0x");for(sh=(int)(sizeof(unsigned long)*8)-4;sh>=0;sh-=4){unsigned n=(unsigned)((v>>sh)&0xf);char c=n<10?(char)('0'+n):(char)('a'+n-10);if(!st&&c=='0'&&sh)continue;st=1;putc(c);}}
static void putdec(long v){unsigned long u=v<0?(unsigned long)-v:(unsigned long)v,d=1;if(v<0)putc('-');while(u/d>=10)d*=10;while(d){putc((char)('0'+(u/d)%10));d/=10;}}

static void lv_touch(void *lv)
{
	unsigned long key=(unsigned long)lv, idx=(key>>4)&255, i;
	for(i=0;i<256;i++){unsigned long j=(idx+i)&255;if(*(lvkey+j)==key||*(lvkey+j)==0){*(lvkey+j)=key;(*(lvcount+j))++;return;}}
}

static void wrap(void *lv, void *sc, void *nr)
{
	calls++;
	lv_touch(lv);
	tramp_fn(lv, sc, nr);
	if (policy_on) {
		unsigned long *n = nr;
		n[2] = 0; n[3] = 0;   /* full-function policy: protect page cache */
	}
}

static int rk_handler(struct kprobe *p, struct pt_regs *r){(void)p;(void)r;refaults++;return 0;}

static u32 br(u32 insn_addr, unsigned long dest, unsigned long src)
{
	long off=(long)(dest-src);
	(void)insn_addr;
	if(off>=(128L<<20)||off<-(128L<<20)) return 0;
	return 0x14000000u | ((u32)(off>>2)&0x03ffffffu);
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
		if(do_patch((void*)target,b)) return -EIO;
	}
	patched=1;
	return 0;
}

static int patch_off(void)
{
	if(!patched) return 0;
	if(do_patch((void*)target,orig0)) return -EIO;
	patched=0;
	return 0;
}

static ssize_t k_read(struct file*f,char __user*buf,size_t len,loff_t*off)
{
	(void)f; kp=kbuf;
	put("ktramp target=");puthex(target);put(" tramp=");puthex((unsigned long)tramp);
	put(" patched=");putdec(patched);put(" policy=");putdec(policy_on);put(" calls=");putdec(calls);
	put(" refaults=");putdec(refaults);put("\n");
	if(!k_srb)return -EIO;
	return k_srb(buf,len,off,kbuf,(size_t)(kp-kbuf));
}

static ssize_t k_write(struct file*f,const char __user*ubuf,size_t len,loff_t*ppos)
{
	char tmp[8];long n;int rc;(void)f;
	if(!k_swb)return -EIO;
	n=k_swb(tmp,sizeof(tmp)-1,ppos,ubuf,len);if(n<0)return n;tmp[n]=0;
	if (tmp[0] == '2') { policy_on = 1; rc = patch_on(); }
	else if (tmp[0] == '1') { policy_on = 0; rc = patch_on(); }
	else { policy_on = 0; rc = patch_off(); }
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
	{
		pc_fn pc=(pc_fn)g_kln("proc_create");
		k_remove=(pr_fn)g_kln("proc_remove");
		k_srb=(srb_fn)g_kln("simple_read_from_buffer");
		k_swb=(swb_fn)g_kln("simple_write_to_buffer");
		if(pc&&k_srb)kpde=pc("ktramp",0644,NULL,&k_pops);
	}
	rkp.pre_handler=rk_handler;
	if(register_kprobe(&rkp)!=0)pr_err("ktramp: refault kprobe failed\n");
	pr_err("ktramp: target=%px patch=%px proc=%d\n",(void*)target,do_patch,kpde!=NULL);
	return 0;
}

static void __exit kt_exit(void)
{
	patch_off();
	if(rkp.pre_handler)unregister_kprobe(&rkp);
	if(kpde&&k_remove)k_remove(kpde);
	pr_err("ktramp: exit\n");
}
module_init(kt_init);module_exit(kt_exit);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("get_scan_count trampoline probe");MODULE_AUTHOR("h2");
