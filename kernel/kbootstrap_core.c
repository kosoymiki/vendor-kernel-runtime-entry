// kbootstrap_core: runtime entry / payload bus for a vendor-frozen Linux kernel.
// Reference profile: RMX3709 5.10.236 (see profiles/). The builder adapts ABI/CFI.
//
// Proven chain (QEMU, exact stock Image):
//   finit_module -> load_module -> do_init_module -> do_one_initcall -> init_module
// The CFI/ABI bridge (kbootstrap_cfi.c + kbootstrap_cfi_jt.S) makes the vendor
// cfi_init() accept this module; postprocess_common.sh + fix_symtab_shndx.py
// normalize the vendor ABI (init 0x190/exit 0x3c0, symtab indices, vermagic,
// image-derived __versions).
//
// This build performs the real payload work:
//   1. resolve kallsyms_lookup_name through register_kprobe() (not exported),
//   2. audit the syscall table entries for the config-disabled kexec/bpf paths,
//   3. allocate executable kernel memory, run a raw AArch64 payload (expect 42),
//   4. expose the observed results through /proc/kbootstrap (side effect, no rmmod).
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#ifndef KB_PROC_NAME
#define KB_PROC_NAME "kbootstrap"
#endif

/* freestanding module build may emit memset for struct kprobe */
void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;

	while (n--)
		*p++ = (unsigned char)c;
	return s;
}

typedef unsigned long (*kallsyms_fn)(const char *);
typedef void *(*alloc_fn)(unsigned long);
typedef void (*flush_fn)(unsigned long, unsigned long);
typedef int (*setmem_fn)(unsigned long, int);
typedef struct proc_dir_entry *(*proc_create_fn)(const char *, umode_t,
						 struct proc_dir_entry *,
						 const struct proc_ops *);
typedef void (*proc_remove_fn)(struct proc_dir_entry *);
typedef long (*srb_fn)(void __user *, size_t, loff_t *, const void *, size_t);

static char kb_buf[2048];
static char *kb_p = kb_buf;
static char *kb_end = kb_buf + sizeof(kb_buf) - 1;
static struct proc_dir_entry *kb_pde;
static proc_remove_fn kb_proc_remove;
static srb_fn kb_srb;

static void putc(char c)
{
	if (kb_p < kb_end)
		*kb_p++ = c;
	*kb_p = 0;
}

static void put(const char *s)
{
	while (*s)
		putc(*s++);
}

static void puthex(unsigned long v)
{
	int shift;
	int started = 0;

	put("0x");
	for (shift = (int)(sizeof(unsigned long) * 8) - 4; shift >= 0; shift -= 4) {
		unsigned int nib = (unsigned int)((v >> shift) & 0xf);
		char c = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);

		if (!started && c == '0' && shift)
			continue;
		started = 1;
		putc(c);
	}
}

static void putdec(long v)
{
	unsigned long u;
	unsigned long d = 1;

	if (v < 0) {
		putc('-');
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	while (u / d >= 10)
		d *= 10;
	while (d) {
		putc((char)('0' + (u / d) % 10));
		d /= 10;
	}
}

static ssize_t kb_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	if (!kb_srb)
		return -EIO;
	return kb_srb(buf, len, off, kb_buf, (size_t)(kb_p - kb_buf));
}

static const struct proc_ops kb_pops = {
	.proc_read = kb_read,
};

static unsigned long sym(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr = 0;

	if (register_kprobe(&kp) == 0) {
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

static __nocfi int run_payload(void *mem)
{
	return ((int (*)(void))mem)();
}

static int __init kb_init(void)
{
	kallsyms_fn kln;
	alloc_fn alloc;
	flush_fn flush;
	setmem_fn setx;
	proc_create_fn proc_create;
	unsigned long sct, ni, mem, rc = 0;
	int setx_rc = -1;
	volatile u32 *code;

#ifdef KB_PROBE_PANIC
	panic("kbootstrap entered");
#endif
	kln = (kallsyms_fn)sym("kallsyms_lookup_name");
	if (!kln) {
		pr_err("kbootstrap: kallsyms_lookup_name unresolved\n");
		return -ENOENT;
	}
	alloc = (alloc_fn)kln("module_alloc");
	flush = (flush_fn)kln("flush_icache_range");
	if (!flush)
		flush = (flush_fn)kln("__flush_icache_range");
	setx = (setmem_fn)kln("set_memory_x");
	proc_create = (proc_create_fn)kln("proc_create");
	kb_proc_remove = (proc_remove_fn)kln("proc_remove");
	kb_srb = (srb_fn)kln("simple_read_from_buffer");
	sct = kln("sys_call_table");
	ni = kln("sys_ni_syscall");
	if (!ni)
		ni = kln("__arm64_sys_ni_syscall");

	put("kmorph runtime entry probe\n");
	put("kallsyms_lookup_name="); puthex((unsigned long)kln); put("\n");
	put("module_alloc="); puthex((unsigned long)alloc); put("\n");
	put("flush_icache_range="); puthex((unsigned long)flush); put("\n");
	put("set_memory_x="); puthex((unsigned long)setx); put("\n");
	put("sys_call_table="); puthex(sct); put("\n");
	put("sys_ni_syscall="); puthex(ni); put("\n");

#ifdef KB_AUDIT_SCT
	if (sct) {
		unsigned long e104 = ((unsigned long *)sct)[104];
		unsigned long e280 = ((unsigned long *)sct)[280];
		unsigned long e294 = ((unsigned long *)sct)[294];

		put("sct[104]="); puthex(e104);
		put(" insn0="); puthex(*(volatile u32 *)e104);
		put(" insn1="); puthex(*(volatile u32 *)(e104 + 4));
		put(" eq_ni="); putdec(e104 == ni); put("\n");
		put("sct[280]="); puthex(e280);
		put(" eq_ni="); putdec(e280 == ni); put("\n");
		put("sct[294]="); puthex(e294);
		put(" insn0="); puthex(*(volatile u32 *)e294);
		put(" insn1="); puthex(*(volatile u32 *)(e294 + 4));
		put(" eq_ni="); putdec(e294 == ni); put("\n");
	}
#endif

	if (alloc && flush) {
		mem = (unsigned long)((alloc_fn)alloc)(PAGE_SIZE);
		if (mem) {
			code = (volatile u32 *)mem;
			code[0] = 0xd503245f; /* bti c */
			code[1] = 0x52800540; /* mov w0, #42 */
			code[2] = 0xd65f03c0; /* ret */
			((flush_fn)flush)(mem, mem + 12);
			if (setx)
				setx_rc = ((setmem_fn)setx)(mem, 1);
			((flush_fn)flush)(mem, mem + 12);
			put("set_memory_x_rc="); putdec(setx_rc); put("\n");
			rc = (unsigned long)run_payload((void *)mem);
			put("payload_result="); putdec((long)rc); put("\n");
		} else {
			put("payload_result=alloc_failed\n");
		}
	} else {
		put("payload_result=unresolved\n");
	}

	if (proc_create && kb_srb) {
		kb_pde = proc_create(KB_PROC_NAME, 0444, NULL, &kb_pops);
		put("proc_entry="); put(kb_pde ? "created" : "failed"); put("\n");
	} else {
		put("proc_entry=unresolved\n");
	}

	pr_err("kbootstrap: ready kln=%px alloc=%px flush=%px sct=%px payload=%lu\n",
	       kln, alloc, flush, (void *)sct, rc);
	return 0;
}

static void __exit kb_exit(void)
{
	if (kb_pde && kb_proc_remove)
		kb_proc_remove(kb_pde);
	pr_err("kbootstrap: exit\n");
}

module_init(kb_init);
module_exit(kb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kmorph runtime entry-point probe");
MODULE_AUTHOR("h2");
