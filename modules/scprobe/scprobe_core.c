#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
static struct kprobe kp = { .symbol_name = "shrink_node" };
static int n;
static int scp_handler(struct kprobe *p, struct pt_regs *r)
{
	void *sc = (void *)r->regs[1];
	(void)p;
	if (n < 6) {
		pr_err("scprobe: sc=%px o2a=%d p2b=%d r2c=%d x2d=%d scan38=%lu recl40=%lu to0=%lu gfp30=%u\n",
		       sc,
		       (int)*(s8 *)((char *)sc + 0x2a), (int)*(s8 *)((char *)sc + 0x2b),
		       (int)*(s8 *)((char *)sc + 0x2c), (int)*(s8 *)((char *)sc + 0x2d),
		       *(unsigned long *)((char *)sc + 0x38),
		       *(unsigned long *)((char *)sc + 0x40),
		       *(unsigned long *)((char *)sc + 0x00),
		       *(unsigned int *)((char *)sc + 0x30));
		n++;
	}
	return 0;
}
static int __init scp_init(void){ kp.pre_handler = scp_handler; return register_kprobe(&kp); }
static void __exit scp_exit(void){ unregister_kprobe(&kp); }
module_init(scp_init); module_exit(scp_exit);
MODULE_LICENSE("GPL");
