/* test_policy.c — userspace unit tests for ktramp_policy.h.
 *
 * Build/run on device (Termux):
 *   clang -O2 -Wall -Wextra -o test_policy tests/test_policy.c && ./test_policy
 *
 * These test the exact inline helpers the kernel module executes, with
 * synthetic memory standing in for struct page (compound_head at offset 8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ktramp_policy.h"

static int fails;

#define CHECK(cond, msg) do { \
	if (cond) { printf("PASS %s\n", msg); } \
	else { printf("FAIL %s\n", msg); fails++; } \
} while (0)

/* 16-byte fake struct page: only compound_head (offset 8) is used. */
static unsigned long *fake_page(unsigned long compound_head)
{
	unsigned long *p = calloc(4, sizeof(unsigned long));

	p[1] = compound_head;	/* offset 8 */
	return p;
}

static void test_folio(void)
{
	unsigned long *p = fake_page(0);
	unsigned long *tail = fake_page((unsigned long)p | 1UL);
	unsigned long heads = 0;

	CHECK(kt_folio_head(0, &heads) == 0 && heads == 0,
	      "folio: null page -> 0");
	CHECK(kt_folio_head((unsigned long)p, &heads) == (unsigned long)p &&
	      heads == 0, "folio: base page unchanged, heads=0");
	CHECK(kt_folio_head((unsigned long)tail, &heads) == (unsigned long)p &&
	      heads == 1, "folio: compound tail -> head, heads=1");
	free(p);
	free(tail);
}

static void test_eviction(void)
{
	enum { N = 8 };
	unsigned long epage[N], egen[N];
	unsigned long p1 = 0x100000, p2 = p1 + N * 4096UL;	/* same index */
	unsigned long p3 = 0x200000;

	memset(epage, 0, sizeof(epage));
	memset(egen, 0, sizeof(egen));

	CHECK(kt_ev_load(epage, egen, N, p1) == 0,
	      "evict: empty table -> 0");
	kt_ev_store(epage, egen, N, p1, 5);
	CHECK(kt_ev_load(epage, egen, N, p1) == 5,
	      "evict: stored generation read back");
	CHECK(kt_ev_load(epage, egen, N, p2) == 0,
	      "evict: index collision -> miss (page compare)");
	kt_ev_store(epage, egen, N, p2, 7);
	CHECK(kt_ev_load(epage, egen, N, p1) == 0 &&
	      kt_ev_load(epage, egen, N, p2) == 7,
	      "evict: overwrite replaces entry");
	kt_ev_store(epage, egen, N, p3, 9);
	CHECK(kt_ev_load(epage, egen, N, p3) == 9,
	      "evict: distinct page stored");
	CHECK(kt_ev_load(epage, egen, 0, p3) == 0,
	      "evict: n=0 -> 0 (no div by zero)");
	kt_ev_store(epage, egen, 0, p3, 9);	/* must not crash */
}

static void test_shadow(void)
{
	unsigned long ev = 123456UL;
	unsigned long shadow = (ev << 19) | (7UL << 3) | (1UL << 2);

	CHECK(kt_shadow_eviction(shadow) == ev,
	      "shadow: eviction timestamp unpacked (>>19)");
	CHECK(kt_shadow_eviction(0) == 0, "shadow: zero -> 0");
	CHECK(kt_refault_distance(1000, 900) == 100,
	      "distance: nonres-eviction");
	CHECK(kt_refault_distance(0, 1) == KT_EVICTION_MASK,
	      "distance: wraps inside 46-bit mask");
	CHECK(kt_refault_distance(5, 5) == 0, "distance: equal -> 0");
}

static void test_buckets(void)
{
	CHECK(kt_dist_bucket(0) == 0, "bucket: 0 -> 0 (no entry)");
	CHECK(kt_dist_bucket(1) == 1, "bucket: 1 -> 1");
	CHECK(kt_dist_bucket(32) == 1, "bucket: 32 -> 1");
	CHECK(kt_dist_bucket(33) == 2, "bucket: 33 -> 2");
	CHECK(kt_dist_bucket(256) == 2, "bucket: 256 -> 2");
	CHECK(kt_dist_bucket(257) == 3, "bucket: 257 -> 3");
}

static void test_policy(void)
{
	unsigned long n[4];
	int br;

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 5, 32, 0, 1, 0, 0, &br);
	CHECK(br == 1 && n[2] == 0 && n[3] == 0 && n[0] == 10 && n[1] == 20,
	      "policy: refault d<=32 zeroes file, keeps anon (branch 1)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 100, 32, 0, 1, 0, 0, &br);
	CHECK(br == 2 && n[2] == 15 && n[3] == 20 && n[0] == 10 && n[1] == 20,
	      "policy: refault d>32 halves file (branch 2)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 1, 1, 0, 0, &br);
	CHECK(br == 3 && n[0] == 0 && n[1] == 0 && n[2] == 30 && n[3] == 40,
	      "policy: odd generation zeroes anon (branch 3)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 0, 1, 0, 0, &br);
	CHECK(br == 4 && n[2] == 15 && n[3] == 20 && n[0] == 10 && n[1] == 20,
	      "policy: even generation halves file (branch 4)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 0, 1, 80, 20, &br);
	CHECK(br == 6 && n[2] == 15 && n[3] == 20 && n[0] == 10 && n[1] == 20,
	      "policy: young working set >60% halves file (branch 6)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 0, 1, 10, 90, &br);
	CHECK(br == 7 && n[2] == 30 && n[3] == 40 && n[0] == 10 && n[1] == 20,
	      "policy: cold pages dominate -> full scan (branch 7)");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 40, 64, 0, 1, 0, 0, &br);
	CHECK(br == 1 && n[2] == 0 && n[3] == 0,
	      "policy: window-scaled threshold d=40 <= 64 zeroes file");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 70, 64, 0, 1, 0, 0, &br);
	CHECK(br == 2 && n[2] == 15 && n[3] == 20,
	      "policy: d=70 > window 64 halves file");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 0, 1, 40, 60, &br);
	CHECK(br == 4 && n[2] == 15 && n[3] == 20,
	      "policy: mixed working set falls back to generation branch");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 5, 32, 0, 0, 0, 0, &br);
	CHECK(br == 1 && n[0] == 0 && n[1] == 0 && n[2] == 0 && n[3] == 0,
	      "policy: no swap does not override refault branch, zeroes anon");

	n[0] = 10; n[1] = 20; n[2] = 30; n[3] = 40;
	kt_policy_nr(n, 0, 32, 0, 0, 0, 0, &br);
	CHECK(br == 4 && n[0] == 0 && n[1] == 0 && n[2] == 15 && n[3] == 20,
	      "policy: no swap keeps file branch (halve), zeroes anon");
}

int main(void)
{
	test_folio();
	test_eviction();
	test_shadow();
	test_buckets();
	test_policy();
	if (fails) {
		printf("RESULT FAIL %d\n", fails);
		return 1;
	}
	printf("RESULT PASS\n");
	return 0;
}
