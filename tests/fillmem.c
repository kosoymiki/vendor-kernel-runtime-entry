/* fillmem.c — freestanding aarch64 helper: touch N MiB of anonymous memory.
 *
 * Why it exists: on a swapless VM the only reclaimable memory is clean file
 * page cache, and normal allocations are satisfied from it silently. Anonymous
 * pages are unreclaimable, so they are what pushes the zone past its watermark
 * and makes the kernel run the file-LRU scanner (`get_scan_count`).
 *
 * Built with no libc (raw syscalls) so the result is a small static ELF that
 * runs on the bare vendor Image inside QEMU:
 *
 *   clang -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
 *         -Wl,-e,_start -o fillmem tests/fillmem.c
 *
 * Usage: ./fillmem [MiB]   (default 128; sleeps 30s with the pages resident)
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

/* A naked entry point: any C prologue would move sp before we read argv, so
 * grab the initial stack pointer in asm and hand it to the real entry. */
void start_c(u64 *sp);

__attribute__((naked, noreturn)) void _start(void) {
    asm volatile("mov x0, sp\n\tb start_c\n");
}

void start_c(u64 *sp) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];
    long mb = argc > 1 ? atoi_(argv[1]) : 128;
    if (mb <= 0) mb = 128;
    u64 n = (u64)mb << 20;
    /* mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    long p = sc(222, 0, (long)n, 3, 0x22, -1, 0);
    if (p < 0) { puts_("fillmem: mmap failed\n"); sc(93, 1, 0, 0, 0, 0, 0); }
    puts_("fillmem: mb="); putdec_(mb);
    puts_(" n="); putdec_((long)n);
    puts_(" p="); putdec_(p);
    puts_("\n");
    for (u64 i = 0; i < n; i += 4096) ((volatile char *)p)[i] = 1;
    puts_("fillmem: touched\n");
    struct { long sec, nsec; } ts = { 30, 0 };
    sc(101, (long)&ts, 0, 0, 0, 0, 0);   /* nanosleep: keep pages resident */
    sc(93, 0, 0, 0, 0, 0, 0);
}
