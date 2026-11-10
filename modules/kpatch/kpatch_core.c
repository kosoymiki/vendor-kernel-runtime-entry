// kpatch_probe: arbitrary kernel text replacement via aarch64_insn_patch_text_nosync.
// Proves the runtime entry for backporting 7.4 functions: save original bytes,
// patch a live function, verify behavior, restore. QEMU only in this probe.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

void *memset(void *s, int c, size_t n) { unsigned char *p=s; while(n--) *p++=(unsigned char)c; return s; }

typedef unsigned long (*kallsyms_fn)(const char *);
typedef struct proc_dir_entry *(*proc_create_fn)(const char *, umode_t, struct proc_dir_entry *, const struct proc_ops *);
typedef void (*proc_remove_fn)(struct proc_dir_entry *);
typedef long (*srb_fn)(void __user *, size_t, loff_t *, const void *, size_t);
typedef long (*swb_fn)(void *, size_t, loff_t *, const void __user *, size_t);
typedef int (*patch_fn)(void *, u32);

static unsigned long target;
static u32 orig[2], patched;
static int is_patched;
static patch_fn do_patch;
static proc_remove_fn k_remove;
static srb_fn k_srb;
static swb_fn k_swb;
static struct proc_dir_entry *kpde;
static char kbuf[512];
static char *kp = kbuf, *kend = kbuf + sizeof(kbuf) - 1;

static void putc(char c){ if(kp<kend)*kp++=c; *kp=0; }
static void put(const char *s){ while(*s) putc(*s++); }
static void puthex(unsigned long v){ int sh,st=0; put("0x"); for(sh=(int)(sizeof(unsigned long)*8)-4; sh>=0; sh-=4){ unsigned n=(unsigned)((v>>sh)&0xf); char c=n<10?(char)('0'+n):(char)('a'+n-10); if(!st&&c=='0'&&sh)continue; st=1; putc(c);} }
static void putdec(long v){ unsigned long u=v<0?(unsigned long)-v:(unsigned long)v,d=1; if(v<0)putc('-'); while(u/d>=10)d*=10; while(d){putc((char)('0'+(u/d)%10)); d/=10;} }

static ssize_t k_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	(void)f;
	kp = kbuf;
	put("kpatch target="); puthex(target);
	put(" patched="); putdec(is_patched); put("\n");
	put("orig0="); puthex(orig[0]); put(" orig1="); puthex(orig[1]); put("\n");
	put("cur0="); puthex(target ? *(volatile u32 *)target : 0); put("\n");
	put("write 1=patch(mov w0,#0; ret) 0=restore\n");
	if (!k_srb) return -EIO;
	return k_srb(buf, len, off, kbuf, (size_t)(kp - kbuf));
}

static int apply(int on)
{
	if (!target || !do_patch) return -EINVAL;
	if (on && !is_patched) {
		orig[0] = *(volatile u32 *)target;
		orig[1] = *(volatile u32 *)(target + 4);
		if (do_patch((void *)target, 0x52800000)) return -EIO; /* mov w0,#0 */
		if (do_patch((void *)(target + 4), 0xd65f03c0)) return -EIO; /* ret */
		is_patched = 1;
	} else if (!on && is_patched) {
		if (do_patch((void *)target, orig[0])) return -EIO;
		if (do_patch((void *)(target + 4), orig[1])) return -EIO;
		is_patched = 0;
	}
	return 0;
}

static ssize_t k_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	char tmp[8]; long n; int rc;
	(void)f;
	if (!k_swb) return -EIO;
	n = k_swb(tmp, sizeof(tmp)-1, ppos, ubuf, len);
	if (n < 0) return n;
	tmp[n] = 0;
	rc = apply(tmp[0] == '1');
	patched++;
	return rc ? rc : n;
}

static const struct proc_ops k_pops = { .proc_read = k_read, .proc_write = k_write };

static unsigned long sym(const char *name)
{
	struct kprobe kp2 = { .symbol_name = name };
	unsigned long a = 0;
	if (register_kprobe(&kp2) == 0) { a = (unsigned long)kp2.addr; unregister_kprobe(&kp2); }
	return a;
}

static int __init k_init(void)
{
	kallsyms_fn kln = (kallsyms_fn)sym("kallsyms_lookup_name");
	proc_create_fn pcreate;
	if (!kln) { pr_err("kpatch: no kallsyms_lookup_name\n"); return -ENOENT; }
	do_patch = (patch_fn)kln("aarch64_insn_patch_text_nosync");
	target = kln("__arm64_sys_getpid");
	pcreate = (proc_create_fn)kln("proc_create");
	k_remove = (proc_remove_fn)kln("proc_remove");
	k_srb = (srb_fn)kln("simple_read_from_buffer");
	k_swb = (swb_fn)kln("simple_write_to_buffer");
	if (pcreate && k_srb)
		kpde = pcreate("kpatch", 0644, NULL, &k_pops);
	pr_err("kpatch: target=%px patch=%px proc=%d\n", (void *)target, do_patch, kpde != NULL);
	return 0;
}

static void __exit k_exit(void)
{
	apply(0);
	if (kpde && k_remove) k_remove(kpde);
	pr_err("kpatch: exit\n");
}

module_init(k_init);
module_exit(k_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("runtime text patch probe");
MODULE_AUTHOR("h2");
