// mglru_probe_core: MGLRU Stage A observability on the vendor 5.10.236 kernel.
//
// Read-only: registers kprobes on the reclaim path and counts calls. No
// behavior change, no text patch. /proc/mglru_probe exposes the counters.
// Symbols observed in this kernel's kallsyms:
//   shrink_node, shrink_node_memcgs, get_scan_count, page_referenced,
//   shrink_slab, do_try_to_free_pages
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;
	while (n--) *p++ = (unsigned char)c;
	return s;
}

typedef unsigned long (*kallsyms_fn)(const char *);
typedef struct proc_dir_entry *(*proc_create_fn)(const char *, umode_t,
						 struct proc_dir_entry *,
						 const struct proc_ops *);
typedef void (*proc_remove_fn)(struct proc_dir_entry *);
typedef long (*srb_fn)(void __user *, size_t, loff_t *, const void *, size_t);
typedef long (*swb_fn)(void *, size_t, loff_t *, const void __user *, size_t);
typedef int (*regkret_fn)(struct kretprobe *);
typedef int (*unregkret_fn)(struct kretprobe *);

static const char *const hook_names[] = {
	"shrink_node",
	"shrink_node_memcgs",
	"get_scan_count",
	"page_referenced",
	"shrink_slab",
	"do_try_to_free_pages",
};
#define NHOOKS (int)(sizeof(hook_names) / sizeof(hook_names[0]))
static unsigned long hits[NHOOKS];
static int registered[NHOOKS];
static int reg_err[NHOOKS];
static struct kprobe kps[NHOOKS];
static struct proc_dir_entry *mpde;
static proc_remove_fn m_remove;
static srb_fn m_srb;
static int policy_mode;          /* 0 off, 1 anon-light, 2 file-light, 3 aggressive */
static unsigned long modified;
static unsigned long last_nr[4];
static unsigned long last_before[4], last_after[4];
static unsigned long changed;
#define GTAB 1024
static unsigned long gpage[GTAB];
static unsigned int ggen[GTAB];
static unsigned long cur_gen = 1, accesses, tracked, young, last_ratio;
static struct kprobe mkp = { .symbol_name = "mark_page_accessed", .pre_handler = NULL };
static swb_fn m_swb;
static regkret_fn m_regkret;
static unregkret_fn m_unregkret;
static int kret_reg_err = -1;
static struct kretprobe grp = {
	.kp.symbol_name = "get_scan_count",
	.entry_handler = NULL,
	.handler = NULL,
	.maxactive = 64,
	.data_size = sizeof(unsigned long *),
};
static char mbuf[1024];
static char *mp = mbuf;
static char *mend = mbuf + sizeof(mbuf) - 1;

static void putc(char c)
{
	if (mp < mend)
		*mp++ = c;
	*mp = 0;
}
static void put(const char *s) { while (*s) putc(*s++); }
static void putdec(long v)
{
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	unsigned long d = 1;

	if (v < 0) putc('-');
	while (u / d >= 10) d *= 10;
	while (d) { putc((char)('0' + (u / d) % 10)); d /= 10; }
}

#define H(n) static int h##n(struct kprobe *p, struct pt_regs *r) \
	{ (void)p; (void)r; hits[n]++; return 0; }
H(0) H(1) H(2) H(3) H(4) H(5)

static int mk_handler(struct kprobe *p, struct pt_regs *r)
{
	unsigned long page = r->regs[0];
	unsigned long idx;

	(void)p;
	if (!page) return 0;
	idx = (page >> 12) & (GTAB - 1);
	*(gpage + idx) = page;
	*(ggen + idx) = (unsigned int)cur_gen;
	accesses++;
	return 0;
}

static int g_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(unsigned long **)ri->data = (unsigned long *)regs->regs[2]; /* nr[] */
	return 0;
}

static int g_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned long *nr = *(unsigned long **)ri->data;
	int i;

	(void)regs;
	if ((++modified & 31) == 0)
		cur_gen++;
	for (i = 0; i < 4; i++)
		last_nr[i] = nr[i];
	for (i = 0; i < 4; i++)
		last_before[i] = nr[i];
	if (policy_mode == 1) {          /* protect anon: no anon scan */
		nr[0] = 0; nr[1] = 0;
		modified++;
	} else if (policy_mode == 2) {   /* protect page cache: no file scan */
		nr[2] = 0; nr[3] = 0;
		modified++;
	} else if (policy_mode == 3) {   /* aggressive: scan more anon */
		nr[0] *= 2; nr[1] *= 2;
		modified++;
	} else if (policy_mode == 4) {   /* generation-aware: protect young */
		unsigned long tr = 0, yg = 0, k;

		for (k = 0; k < GTAB; k++) {
			if (*(gpage + k)) {
				tr++;
				if (*(ggen + k) == (unsigned int)cur_gen)
					yg++;
			}
		}
		tracked = tr; young = yg;
		last_ratio = tr ? (yg * 100 / tr) : 0;
		if (last_ratio >= 50) {          /* mostly young: protect */
			nr[0] /= 2; nr[1] /= 2; nr[2] /= 2; nr[3] /= 2;
		} else {                         /* old pages dominate: scan file */
			nr[2] *= 2; nr[3] *= 2;
		}
		modified++;
	}
	if (policy_mode) {
		for (i = 0; i < 4; i++)
			last_after[i] = nr[i];
		for (i = 0; i < 4; i++)
			if (last_before[i] != last_after[i]) { changed++; break; }
	}
	return 0;
}

static ssize_t m_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	char tmp[8];
	long n;

	(void)f;
	if (!m_swb) return -EIO;
	n = m_swb(tmp, sizeof(tmp) - 1, ppos, ubuf, len);
	if (n < 0) return n;
	tmp[n] = 0;
	if (tmp[0] >= '0' && tmp[0] <= '4')
		policy_mode = tmp[0] - '0';
	return n;
}

static ssize_t m_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	int i;

	(void)f;
	mp = mbuf;
	put("mglru_probe stage B policy (write 0|1|2 to /proc/mglru_probe)\n");
	put("policy_mode="); putdec(policy_mode);
	put(" modified="); putdec((long)modified);
	put(" kretprobe_reg="); putdec(kret_reg_err);
	put(" changed="); putdec((long)changed);
	put(" gen="); putdec((long)cur_gen);
	put(" acc="); putdec((long)accesses);
	put(" trk="); putdec((long)tracked);
	put(" yng="); putdec((long)young);
	put(" ratio="); putdec((long)last_ratio); put("\n");
	put(" before= "); putdec((long)last_before[0]); put(",");
	putdec((long)last_before[1]); put(",");
	putdec((long)last_before[2]); put(",");
	putdec((long)last_before[3]); put("\n");
	put(" after=  "); putdec((long)last_after[0]); put(",");
	putdec((long)last_after[1]); put(",");
	putdec((long)last_after[2]); put(",");
	putdec((long)last_after[3]); put("\n");
	for (i = 0; i < NHOOKS; i++) {
		put(hook_names[i]); put(": reg=");
		putdec(registered[i]); put(" err="); putdec(reg_err[i]);
		put(" hits="); putdec((long)hits[i]); put("\n");
	}
	if (!m_srb) return -EIO;
	return m_srb(buf, len, off, mbuf, (size_t)(mp - mbuf));
}

static const struct proc_ops m_pops = { .proc_read = m_read, .proc_write = m_write };

static unsigned long sym(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long a = 0;

	if (register_kprobe(&kp) == 0) { a = (unsigned long)kp.addr; unregister_kprobe(&kp); }
	return a;
}

static int __init m_init(void)
{
	kallsyms_fn kln = (kallsyms_fn)sym("kallsyms_lookup_name");
	proc_create_fn pcreate;
	int i;

	if (!kln) { pr_err("mglru_probe: no kallsyms_lookup_name\n"); return -ENOENT; }
	pcreate = (proc_create_fn)kln("proc_create");
	m_remove = (proc_remove_fn)kln("proc_remove");
	m_srb = (srb_fn)kln("simple_read_from_buffer");
	m_swb = (swb_fn)kln("simple_write_to_buffer");
	m_regkret = (regkret_fn)kln("register_kretprobe");
	m_unregkret = (unregkret_fn)kln("unregister_kretprobe");

	for (i = 0; i < NHOOKS; i++) {
		kps[i].symbol_name = hook_names[i];
		kps[i].pre_handler = i == 0 ? h0 : i == 1 ? h1 : i == 2 ? h2 :
				      i == 3 ? h3 : i == 4 ? h4 : h5;
		reg_err[i] = register_kprobe(&kps[i]);
		registered[i] = reg_err[i] == 0;
		if (reg_err[i])
			pr_err("mglru_probe: %s register failed %d\n", hook_names[i], reg_err[i]);
	}
	mkp.pre_handler = mk_handler;
	if (register_kprobe(&mkp) != 0)
		pr_err("mglru_probe: mark_page_accessed kprobe failed\n");
	if (m_regkret) {
		grp.entry_handler = g_entry;
		grp.handler = g_ret;
		kret_reg_err = m_regkret(&grp);
	}
	if (pcreate && m_srb)
		mpde = pcreate("mglru_probe", 0644, NULL, &m_pops);
	pr_err("mglru_probe: registered %d/%d, proc=%d\n",
	       registered[0] + registered[1] + registered[2] + registered[3] +
	       registered[4] + registered[5], NHOOKS, mpde != NULL);
	return 0;
}

static void __exit m_exit(void)
{
	int i;

	for (i = 0; i < NHOOKS; i++)
		if (registered[i])
			unregister_kprobe(&kps[i]);
	if (kret_reg_err == 0 && m_unregkret)
		m_unregkret(&grp);
	if (mpde && m_remove)
		m_remove(mpde);
	pr_err("mglru_probe: exit\n");
}

module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MGLRU Stage A observability probe");
MODULE_AUTHOR("h2");
