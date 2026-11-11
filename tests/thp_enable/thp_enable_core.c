// thp_enable: test helper. The vendor kernel compiles out enabled_store()
// under CONFIG_CONT_PTE_HUGEPAGE, so writing madvise to sysfs is a no-op.
// Set the global transparent_hugepage_flags bit directly (QEMU-only helper).
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
typedef unsigned long (*kallsyms_fn)(const char *);
static unsigned long *flags;
static int __init te_init(void)
{
	struct kprobe k = { .symbol_name = "kallsyms_lookup_name" };
	kallsyms_fn kln;
	if (register_kprobe(&k) != 0)
		return -ENOENT;
	kln = (kallsyms_fn)k.addr;
	unregister_kprobe(&k);
	flags = (unsigned long *)kln("transparent_hugepage_flags");
	if (!flags)
		return -ENOENT;
	set_bit(1, flags); set_bit(0, flags);	/* madvise + always */
	clear_bit(0, flags);	/* TRANSPARENT_HUGEPAGE_FLAG */
	pr_info("thp_enable: flags=%lx\n", *flags);
	return 0;
}
static void __exit te_exit(void)
{
	if (flags)
		clear_bit(1, flags);
	pr_info("thp_enable: exit\n");
}
module_init(te_init); module_exit(te_exit);
MODULE_LICENSE("GPL");
