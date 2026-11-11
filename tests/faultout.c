/* faultout.c — freestanding aarch64 helper: populate a small file range,
 * page it out with MADV_PAGEOUT, then fault it back to create refaults with
 * a *small* refault distance (the kernel shadow timestamp advances by the
 * number of evicted pages).
 *
 * Built like fillmem (raw syscalls, no libc):
 *   clang -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
 *         -Wl,-e,_start -o faultout tests/faultout.c
 *
 * Usage: ./faultout <path> <offset_kb> <len_kb>
 */
typedef unsigned long u64;

static inline long sc(long n, long a, long b, long c, long d, long e, long f) {
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x4 asm("x4") = e;
	register long x5 asm("x5") = f;
	asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static long atoi_(const char *s) {
	long v = 0;
	if (!s) return 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

static void puts_(const char *s) {
	long n = 0;
	while (s[n]) n++;
	sc(64, 1, (long)s, n, 0, 0, 0);
}

static void putdec_(long v) {
	char b[24];
	int i = 0;
	if (v < 0) { puts_("-"); v = -v; }
	if (!v) b[i++] = '0';
	while (v > 0 && i < 23) { b[i++] = (char)('0' + v % 10); v /= 10; }
	while (i > 0) sc(64, 1, (long)&b[--i], 1, 0, 0, 0);
}

void start_c(u64 *sp);

__attribute__((naked, noreturn)) void _start(void) {
	asm volatile("mov x0, sp\n\tb start_c\n");
}

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define PROT_READ 1
#define MAP_PRIVATE 2
#define MADV_PAGEOUT 21

void start_c(u64 *sp) {
	long argc = (long)sp[0];
	char **argv = (char **)&sp[1];
	const char *path = argc > 1 ? argv[1] : "/mnt/big";
	long off_kb = argc > 2 ? atoi_(argv[2]) : 0;
	long len_kb = argc > 3 ? atoi_(argv[3]) : 128;
	long fd, p, i;
	u64 len;

	if (len_kb <= 0) len_kb = 128;
	len = (u64)len_kb << 10;

	fd = sc(56, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);	/* openat */
	if (fd < 0) { puts_("faultout: open failed "); putdec_(fd); puts_("\n"); sc(93, 1, 0, 0, 0, 0, 0); }

	p = sc(222, 0, (long)len, PROT_READ, MAP_PRIVATE, fd, off_kb << 10); /* mmap */
	if (p < 0) { puts_("faultout: mmap failed "); putdec_(p); puts_("\n"); sc(93, 1, 0, 0, 0, 0, 0); }

	for (i = 0; i < (long)len; i += 4096)
		(void)*(volatile char *)(p + i);
	puts_("faultout: populated kb="); putdec_(len_kb);
	puts_(" off_kb="); putdec_(off_kb); puts_("\n");

	/* MADV_PAGEOUT: evict exactly this range (file must be writable). */
	{
		long r = sc(233, p, (long)len, MADV_PAGEOUT, 0, 0, 0);
		puts_("faultout: madvise_pageout rc="); putdec_(r); puts_("\n");
	}

	/* Fault the range back: each page is a refault with distance <= len pages. */
	for (i = 0; i < (long)len; i += 4096)
		(void)*(volatile char *)(p + i);
	puts_("faultout: refaulted\n");

	sc(215, p, (long)len, 0, 0, 0, 0);	/* munmap */
	sc(57, fd, 0, 0, 0, 0, 0);		/* close */
	sc(93, 0, 0, 0, 0, 0, 0);
}
