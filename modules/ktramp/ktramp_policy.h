/* ktramp_policy.h — pure, kernel-independent policy helpers for ktramp.
 *
 * Extracted so the exact code the module runs can be unit-tested in
 * userspace (tests/test_policy.c) without a kernel build or QEMU:
 *   - folio head normalization (compound tail -> head)
 *   - page->generation shadow table (eviction/refault distance)
 *   - scan-count policy branches (refault distance / generation)
 *
 * No kernel headers, no globals. Callers pass their own arrays.
 */
#ifndef KTRAMP_POLICY_H
#define KTRAMP_POLICY_H

/* page->compound_head on this kernel is at offset 8; bit 0 set means the
 * value is (head + 1). See include/linux/page-flags.h:192. */
#define KT_COMPOUND_HEAD_OFF 8UL

static inline unsigned long kt_folio_head(unsigned long page,
					  unsigned long *heads)
{
	unsigned long h;

	if (!page)
		return 0;
	h = *(volatile unsigned long *)(page + KT_COMPOUND_HEAD_OFF);
	if (h & 1UL) {
		if (heads)
			(*heads)++;
		return h - 1UL;
	}
	return page;
}

/* Shadow table: page -> generation. A zero generation means "no entry". */
static inline void kt_ev_store(unsigned long *epage, unsigned long *egen,
			       unsigned long n, unsigned long page,
			       unsigned long gen)
{
	unsigned long idx;

	if (!n)
		return;
	idx = (page >> 12) & (n - 1);
	epage[idx] = page;
	egen[idx] = gen;
}

static inline unsigned long kt_ev_load(const unsigned long *epage,
				       const unsigned long *egen,
				       unsigned long n, unsigned long page)
{
	unsigned long idx;

	if (!n)
		return 0;
	idx = (page >> 12) & (n - 1);
	if (epage[idx] == page)
		return egen[idx];
	return 0;
}

/* Kernel shadow timestamp (mm/workingset.c unpack_shadow):
 * xa_to_value = shadow >> 2, workingset bit = >> 1, NODES_SHIFT = 0,
 * MEM_CGROUP_ID_SHIFT = 16, bucket_order = 0 on this kernel
 * => eviction = shadow >> 19. */
#define KT_SHADOW_EVICTION_SHIFT 19UL

static inline unsigned long kt_shadow_eviction(unsigned long shadow)
{
	return shadow >> KT_SHADOW_EVICTION_SHIFT;
}

/* EVICTION_MASK = ~0UL >> EVICTION_SHIFT, EVICTION_SHIFT = 18 on this kernel
 * (BITS_PER_LONG 64, BITS_PER_XA_VALUE 63, NODES_SHIFT 0,
 *  MEM_CGROUP_ID_SHIFT 16) => 46 bits of timestamp. */
#define KT_EVICTION_MASK ((1UL << 46) - 1)

static inline unsigned long kt_refault_distance(unsigned long nonres,
						unsigned long eviction)
{
	return (nonres - eviction) & KT_EVICTION_MASK;
}

/* Adaptive min_ttl analogue: raise the window when refaults dominate
 * evictions (thrashing), lower it when they do not. Range [2,4]. */
static inline unsigned long kt_ttl_step(unsigned long ev, unsigned long rf,
					unsigned long ttl)
{
	unsigned long thrash = ev ? (rf * 100 / ev) : 0;

	if (!ttl)
		ttl = 2;
	if (thrash > 50 && ttl < 4)
		ttl++;
	else if (thrash < 10 && ttl > 2)
		ttl--;
	return ttl;
}

/* Refault distance bucket: 0 = no entry, 1 = 1..32, 2 = 33..256, 3 = >256. */
static inline int kt_dist_bucket(unsigned long dist)
{
	if (!dist)
		return 0;
	if (dist <= 32)
		return 1;
	if (dist <= 256)
		return 2;
	return 3;
}

/* Scan-count policy.
 *   d     = last refault distance recorded for this lruvec (0 = none),
 *           from the kernel shadow timestamp and nonresident_age
 *   gen   = per-memcg (or per-lruvec fallback) generation parity
 *   swap  = anon pages are reclaimable (nr_swap_pages > 0)
 *
 * Branches (branch out-param, for counters):
 *   1: refault would activate (d <= 32)  -> zero file scan
 *   2: refault, larger distance          -> halve file scan
 *   3: generation odd                    -> zero anon scan
 *   4: generation even                   -> halve file scan
 *   5: no swap                           -> zero anon scan (unreclaimable)
 *
 * MGLRU's can_swap check: without swap, scanning anon is wasted work, so the
 * no-swap branch wins over refault/generation.
 */
static inline void kt_policy_nr(unsigned long *n, unsigned long d,
				unsigned long d_small, int gen,
				int swap, unsigned long young, unsigned long cold,
				unsigned long age_old, unsigned long age_all,
				int *branch)
{
	if (d) {
		if (d <= (d_small ? d_small : 32)) {
			n[2] = 0;
			n[3] = 0;
			*branch = 1;
		} else {
			n[2] /= 2;
			n[3] /= 2;
			*branch = 2;
		}
	} else if (young + cold > 0 && young * 100 / (young + cold) > 60) {
		/* Young working set dominates: protect it, scan file less. */
		n[2] /= 2;
		n[3] /= 2;
		*branch = 6;
	} else if (young + cold > 0 && young * 100 / (young + cold) < 20) {
		/* Cold pages dominate: keep full scan (evict oldest first). */
		*branch = 7;
	} else if (age_all > 0 && age_old * 100 / age_all > 70) {
		/* Evictions are old pages: full scan. */
		*branch = 8;
	} else if (age_all > 0 && age_old * 100 / age_all < 30) {
		/* Evictions are young pages: protect the working set. */
		n[2] /= 2;
		n[3] /= 2;
		*branch = 9;
	} else if (gen) {
		n[0] = 0;
		n[1] = 0;
		*branch = 3;
	} else {
		n[2] /= 2;
		n[3] /= 2;
		*branch = 4;
	}
	/* can_swap only gates anon scanning; it never overrides the file
	 * policy above. Without swap the kernel already zeroes anon counts
	 * (mm/vmscan.c:2522), so this is normally a no-op. */
	if (!swap) {
		n[0] = 0;
		n[1] = 0;
	}
}

#endif /* KTRAMP_POLICY_H */
