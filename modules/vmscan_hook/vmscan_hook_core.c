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
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/page-flags.h>

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
static unsigned long calls, scans, reclaimed_pages, moved, ttl_ms = 50;
static unsigned long sb[4], compound;

/* shrink_node_memcgs entry hook: per-memcg scan function */
static unsigned long sn_target, sn_calls, sn_boost;
static u32 sn_orig0;
static void *sn_tramp;
static int sn_patched;
static void *(*g_module_alloc)(unsigned long);
static int (*g_set_memory_x)(unsigned long, int);
static void (*g_flush_icache)(unsigned long, unsigned long);
static void (*sn_orig_fn)(void *, void *);
static patch_fn do_patch;
static struct proc_dir_entry *vpde;
static char vbuf[512];
static char *vp = vbuf, *vend = vbuf + sizeof(vbuf) - 1;

static get_scan_fn g_get_scan;
static shrink_inactive_fn g_shrink_inactive;
static shrink_active_fn g_shrink_active;
static cond_resched_fn g_cond_resched;
static unsigned long *jiffies_p;

#define GENN 8192
static unsigned long gpage[GENN], gtime[GENN];
static struct kprobe akp = { .symbol_name = "mark_page_accessed" };
static int ak_reg;

static void g_store(unsigned long page, unsigned long t)
{
	struct page *p = compound_head((struct page *)page);
	unsigned long i = ((unsigned long)p >> 12) & (GENN - 1);

	gpage[i] = (unsigned long)p;
	gtime[i] = t;
}

static unsigned long g_load(unsigned long page)
{
	struct page *p = compound_head((struct page *)page);
	unsigned long i = ((unsigned long)p >> 12) & (GENN - 1);

	return gpage[i] == (unsigned long)p ? gtime[i] : 0;
}

static int a_handler(struct kprobe *p, struct pt_regs *r)
{
	unsigned long page = r->regs[0];

	(void)p;
	if (page && jiffies_p)
		g_store(page, *jiffies_p);
	return 0;
}

/* MGLRU-style list ordering: bucket inactive pages by age (0,1,2,3+ ttl
 * windows) and rebuild each list oldest-first, so the kernel scanner (which
 * takes from the head) evicts the oldest generation first. Under lru_lock. */
static void rotate_young(struct lruvec *lv)
{
	struct pglist_data *pgdat = lv->pgdat;
	struct page *page, *tmp;
	unsigned long flags, now, ttl;
	int lru, b;

	if (!pgdat || !jiffies_p || !ttl_ms)
		return;
	now = *jiffies_p;
	ttl = ttl_ms * HZ / 1000;
	if (!ttl)
		ttl = 1;
	spin_lock_irqsave(&pgdat->lru_lock, flags);
	for (lru = 0; lru <= 3; lru++) {
		LIST_HEAD(b0); LIST_HEAD(b1); LIST_HEAD(b2); LIST_HEAD(b3);
		LIST_HEAD(ord);

		list_for_each_entry_safe(page, tmp, &lv->lists[lru], lru) {
			unsigned long t = g_load((unsigned long)page);
			unsigned long age = t ? now - t : ~0UL;

			if (PageCompound(page))
				compound++;
			if (age < ttl) b = 0;
			else if (age < 2 * ttl) b = 1;
			else if (age < 4 * ttl) b = 2;
			else b = 3;
			switch (b) {
			case 0: list_move(&page->lru, &b0); break;
			case 1: list_move(&page->lru, &b1); break;
			case 2: list_move(&page->lru, &b2); break;
			default: list_move(&page->lru, &b3); break;
			}
			moved++;
			sb[b]++;
		}
		list_splice_tail(&b3, &ord);
		list_splice_tail(&b2, &ord);
		list_splice_tail(&b1, &ord);
		list_splice_tail(&b0, &ord);
		list_splice(&ord, &lv->lists[lru]);
	}
	spin_unlock_irqrestore(&pgdat->lru_lock, flags);
}


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
	rotate_young((struct lruvec *)lruvec);
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

static u32 br(u32 insn_addr, unsigned long dest, unsigned long src)
{
	long off = (long)(dest - src);
	(void)insn_addr;
	if (off >= (128L << 20) || off < -(128L << 20))
		return 0;
	return 0x14000000u | ((u32)(off >> 2) & 0x03ffffffu);
}

static noinline void my_sn_memcgs(void *pgdat, void *sc)
{
	sn_calls++;
	if (sn_boost)
		*(s8 *)((char *)sc + 0x2b) = 0;	/* sc->priority */
	sn_orig_fn(pgdat, sc);
}

static int sn_patch_on(void)
{
	u32 *tp;
	int i;
	u32 b;
	void *a[1];
	u32 ins[1];

	if (sn_patched)
		return 0;
	if (!sn_target || !do_patch)
		return -EINVAL;
	sn_orig0 = *(volatile u32 *)sn_target;
	if (!sn_tramp) {
		if (!g_module_alloc || !g_set_memory_x || !g_flush_icache)
			return -ENOENT;
		sn_tramp = g_module_alloc(PAGE_SIZE);
		if (!sn_tramp)
			return -ENOMEM;
		tp = (u32 *)sn_tramp;
		for (i = 0; i < 7; i++)
			tp[i] = *(volatile u32 *)(sn_target + i * 4);
		tp[7] = br(0, (unsigned long)(sn_target + 28), (unsigned long)(sn_tramp)+28);
		if (!tp[7])
			return -ERANGE;
		g_flush_icache((unsigned long)sn_tramp, (unsigned long)sn_tramp + 32);
		g_set_memory_x((unsigned long)sn_tramp, 1);
		g_flush_icache((unsigned long)sn_tramp, (unsigned long)sn_tramp + 32);
	}
	sn_orig_fn = (void (*)(void *, void *))sn_tramp;
	b = br(0, (unsigned long)my_sn_memcgs, sn_target);
	if (!b)
		return -ERANGE;
	a[0] = (void *)sn_target; ins[0] = b;
	if (do_patch(a, ins, 1))
		return -EIO;
	sn_patched = 1;
	return 0;
}

static int sn_patch_off(void)
{
	void *a[1];
	u32 ins[1];

	if (!sn_patched)
		return 0;
	a[0] = (void *)sn_target; ins[0] = sn_orig0;
	if (do_patch(a, ins, 1))
		return -EIO;
	sn_patched = 0;
	return 0;
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
	put_(" sn_hooked="); putdec_(sn_patched); put_(" sn_calls="); putdec_((long)sn_calls); put_(" sn_boost="); putdec_(sn_boost);
	put_(" reclaimed="); putdec_((long)reclaimed_pages); put_(" moved="); putdec_((long)moved); put_(" compound="); putdec_((long)compound); put_(" buckets="); putdec_((long)sb[0]); put_(","); putdec_((long)sb[1]); put_(","); putdec_((long)sb[2]); put_(","); putdec_((long)sb[3]); put_(" ttl_ms="); putdec_((long)ttl_ms);
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
	if (tmp[0] == 'm') {
		sn_boost = 1;
		rc = sn_patch_on();
	} else if (tmp[0] == 't') {
		char *q = tmp + 1;
		unsigned long v = 0;
		while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
		ttl_ms = v;
	} else if (tmp[0] == '1') {
		rc = patch_on();
	} else {
		sn_patch_off();
		rc = patch_off();
	}
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
	sn_target = kln("shrink_node_memcgs");
	g_module_alloc = (void *(*)(unsigned long))kln("module_alloc");
	g_set_memory_x = (int (*)(unsigned long, int))kln("set_memory_x");
	g_flush_icache = (void (*)(unsigned long, unsigned long))kln("__flush_icache_range");
	jiffies_p = (unsigned long *)kln("jiffies");
	if (!target || !g_get_scan || !g_shrink_inactive || !g_shrink_active || !do_patch) {
		pr_err("vmscan_hook: missing symbols target=%px gs=%px si=%px sa=%px p=%px\n",
		       (void *)target, (void *)g_get_scan, (void *)g_shrink_inactive,
		       (void *)g_shrink_active, (void *)do_patch);
		return -ENOENT;
	}
	akp.pre_handler = a_handler;
	if (register_kprobe(&akp) == 0)
		ak_reg = 1;
	else
		pr_warn("vmscan_hook: mark_page_accessed kprobe failed\n");
	vpde = proc_create("vmscan_hook", 0644, NULL, &v_pops);
	pr_info("vmscan_hook: ready target=%px\n", (void *)target);
	return 0;
}

static void __exit vh_exit(void)
{
	sn_patch_off();
	patch_off();
	if (ak_reg)
		unregister_kprobe(&akp);
	if (vpde)
		proc_remove(vpde);
	pr_info("vmscan_hook: exit\n");
}

module_init(vh_init);
module_exit(vh_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("custom shrink_lruvec via module text patch");
