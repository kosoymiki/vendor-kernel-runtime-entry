// vmscan_hook: replace shrink_lruvec with a module implementation that runs
// the kernel's own shrink_active_list/shrink_inactive_list, so a custom
// reclaim policy is injected without rebuilding vmscan.
//
// Offsets are verified on the target by scprobe in QEMU (shrink_node entry):
//   nr_to_reclaim 0x00, bits 0x28, gfp_mask 0x30, nr_scanned 0x38,
//   nr_reclaimed 0x40, order 0x2a, priority 0x2b (12->11->10), reclaim_idx 0x2c.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/stddef.h>

#define SC_NR_TO_RECLAIM 0x00UL
#define SC_BITS          0x28UL
#define SC_GFP_MASK      0x30UL
#define SC_NR_SCANNED    0x38UL
#define SC_NR_RECLAIMED  0x40UL
#define SC_SKIPPED_DEACT (1u << 3)
#define SWAP_CLUSTER_MAX 32UL

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;

	while (n--)
		*p++ = (unsigned char)c;
	return s;
}

typedef unsigned long (*kallsyms_fn)(const char *);
typedef unsigned long (*get_scan_fn)(void *, void *, unsigned long *);
typedef unsigned long (*shrink_inactive_fn)(unsigned long, void *, void *, int);
typedef void (*shrink_active_fn)(unsigned long, void *, void *, int);
typedef void (*cond_resched_fn)(void);
typedef int (*patch_fn)(void **, u32 *, int);

static unsigned long target;		/* shrink_lruvec */
static u32 orig0;
static int hooked;
static unsigned long calls, scans, reclaimed_pages;
static patch_fn do_patch;
static struct proc_dir_entry *vpde;
static char vbuf[512];
static char *vp = vbuf, *vend = vbuf + sizeof(vbuf) - 1;

static get_scan_fn g_get_scan;
static shrink_inactive_fn g_shrink_inactive;
static shrink_active_fn g_shrink_active;
static cond_resched_fn g_cond_resched;

static void putc_(char c) { if (vp < vend) *vp++ = c; *vp = 0; }
static void put_(const char *s) { while (*s) putc_(*s++); }
static void putdec_(long v)
{
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v, d = 1;

	if (v < 0) putc_('-');
	while (u / d >= 10) d *= 10;
	while (d) { putc_((char)('0' + (u / d) % 10)); d /= 10; }
}

/* The custom reclaim loop: same primitives as vmscan's shrink_lruvec, but
 * the scan order/limits are ours and every call is visible in /proc. */
static noinline void my_shrink_lruvec(void *lruvec, void *sc)
{
	unsigned long nr[4] = { 0, 0, 0, 0 };
	unsigned long nr_to = *(unsigned long *)((char *)sc + SC_NR_TO_RECLAIM);
	unsigned long r0 = *(unsigned long *)((char *)sc + SC_NR_RECLAIMED);
	int lru, guard = 0;

	calls++;
	g_get_scan(lruvec, sc, nr);
	while ((nr[0] || nr[2] || nr[3]) && guard++ < 4096) {
		for (lru = 0; lru <= 3; lru++) {
			unsigned long n;

			if (!nr[lru])
				continue;
			n = nr[lru] > SWAP_CLUSTER_MAX ? SWAP_CLUSTER_MAX : nr[lru];
			nr[lru] -= n;
			scans += n;
			if (lru == 1 || lru == 3) {
				unsigned int bit = (lru == 3) ? 1u : 0u;
				unsigned int bits = *(unsigned int *)((char *)sc + SC_BITS);

				if (bits & (1u << bit))
					g_shrink_active(n, lruvec, sc, lru);
				else
					*(unsigned int *)((char *)sc + SC_BITS) |= SC_SKIPPED_DEACT;
			} else {
				g_shrink_inactive(n, lruvec, sc, lru);
			}
		}
		if (*(unsigned long *)((char *)sc + SC_NR_RECLAIMED) - r0 >= nr_to)
			break;
		if (g_cond_resched)
			g_cond_resched();
	}
	reclaimed_pages = *(unsigned long *)((char *)sc + SC_NR_RECLAIMED) - r0;
}

static int patch_on(void)
{
	u32 insn;
	long off;
	void *a[1];
	u32 i[1];

	if (hooked)
		return 0;
	if (!target || !do_patch)
		return -EINVAL;
	orig0 = *(volatile u32 *)target;
	off = (long)((unsigned long)my_shrink_lruvec - target);
	if (off >= (128L << 20) || off < -(128L << 20))
		return -ERANGE;
	insn = 0x14000000u | ((u32)(off >> 2) & 0x03ffffffu);
	a[0] = (void *)target;
	i[0] = insn;
	if (do_patch(a, i, 1))
		return -EIO;
	hooked = 1;
	return 0;
}

static int patch_off(void)
{
	void *a[1];
	u32 i[1];

	if (!hooked)
		return 0;
	a[0] = (void *)target;
	i[0] = orig0;
	if (do_patch(a, i, 1))
		return -EIO;
	hooked = 0;
	return 0;
}

static ssize_t v_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	(void)f;
	vp = vbuf;
	put_("vmscan_hook target=");
	putdec_((long)target);
	put_(" hooked="); putdec_(hooked);
	put_(" calls="); putdec_((long)calls);
	put_(" scans="); putdec_((long)scans);
	put_(" reclaimed="); putdec_((long)reclaimed_pages);
	put_("\n");
	return simple_read_from_buffer(buf, len, off, vbuf, (size_t)(vp - vbuf));
}

static ssize_t v_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	char tmp[8];
	long n;
	int rc = 0;

	(void)f;
	n = simple_write_to_buffer(tmp, sizeof(tmp) - 1, ppos, ubuf, len);
	if (n < 0)
		return n;
	tmp[n] = 0;
	if (tmp[0] == '1')
		rc = patch_on();
	else
		rc = patch_off();
	return rc ? rc : n;
}

static const struct proc_ops v_pops = { .proc_read = v_read, .proc_write = v_write };

static unsigned long sym(const char *name)
{
	struct kprobe k = { .symbol_name = name };
	unsigned long a = 0;

	if (register_kprobe(&k) == 0) {
		a = (unsigned long)k.addr;
		unregister_kprobe(&k);
	}
	return a;
}

static int __init vh_init(void)
{
	kallsyms_fn kln;

	kln = (kallsyms_fn)sym("kallsyms_lookup_name");
	if (!kln) {
		pr_err("vmscan_hook: no kallsyms_lookup_name\n");
		return -ENOENT;
	}
	target = kln("shrink_lruvec");
	g_get_scan = (get_scan_fn)kln("get_scan_count");
	g_shrink_inactive = (shrink_inactive_fn)kln("shrink_inactive_list");
	g_shrink_active = (shrink_active_fn)kln("shrink_active_list");
	g_cond_resched = (cond_resched_fn)kln("cond_resched");
	do_patch = (patch_fn)kln("aarch64_insn_patch_text");
	if (!target || !g_get_scan || !g_shrink_inactive || !g_shrink_active || !do_patch) {
		pr_err("vmscan_hook: missing symbols target=%px gs=%px si=%px sa=%px p=%px\n",
		       (void *)target, (void *)g_get_scan, (void *)g_shrink_inactive,
		       (void *)g_shrink_active, (void *)do_patch);
		return -ENOENT;
	}
	vpde = proc_create("vmscan_hook", 0644, NULL, &v_pops);
	pr_info("vmscan_hook: ready target=%px\n", (void *)target);
	return 0;
}

static void __exit vh_exit(void)
{
	patch_off();
	if (vpde)
		proc_remove(vpde);
	pr_info("vmscan_hook: exit\n");
}

module_init(vh_init);
module_exit(vh_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("custom shrink_lruvec via module text patch");
