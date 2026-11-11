/* thpfill.c — freestanding aarch64 helper: allocate N MiB of anonymous
 * memory with MADV_HUGEPAGE and touch it, so compound (THP) pages reach the
 * LRU lists. Used by the vmscan_hook folio gate.
 *
 * clang -O2 -static -nostdlib -fno-stack-protector -ffreestanding \
 *       -Wl,-e,_start -o thpfill tests/thpfill.c
 */
typedef unsigned long u64;
static inline long sc(long n,long a,long b,long c,long d,long e,long f){
 register long x8 asm("x8")=n; register long x0 asm("x0")=a; register long x1 asm("x1")=b;
 register long x2 asm("x2")=c; register long x3 asm("x3")=d; register long x4 asm("x4")=e;
 register long x5 asm("x5")=f;
 asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
 return x0;
}
static long atoi_(const char*s){long v=0;if(!s)return 0;while(*s>='0'&&*s<='9')v=v*10+(*s++-'0');return v;}
static void puts_(const char*s){long n=0;while(s[n])n++;sc(64,1,(long)s,n,0,0,0);}
static void putdec_(long v){char b[24];int i=0;if(v<0){puts_("-");v=-v;}if(!v)b[i++]='0';while(v>0&&i<23){b[i++]=(char)('0'+v%10);v/=10;}while(i>0)sc(64,1,(long)&b[--i],1,0,0,0);}
void start_c(u64*sp);
__attribute__((naked,noreturn)) void _start(void){asm volatile("mov x0, sp\n\tb start_c\n");}
void start_c(u64*sp){
 long argc=(long)sp[0]; char**argv=(char**)&sp[1];
 long mb=argc>1?atoi_(argv[1]):8; u64 n=(u64)mb<<20; long p; u64 i;
 if(mb<=0)mb=8;
 p=sc(222,0,(long)n,3,0x22,-1,0);
 if(p<0){puts_("thpfill: mmap failed\n");sc(93,1,0,0,0,0,0);}
 sc(233,p,(long)n,14,0,0,0); /* MADV_HUGEPAGE */
 for(i=0;i<n;i+=4096)(void)*(volatile char*)(p+i);
 puts_("thpfill: mb=");putdec_(mb);puts_(" p=");putdec_(p);puts_("\n");
 { struct { long sec,nsec; } ts={30,0}; sc(101,(long)&ts,0,0,0,0,0); }
 sc(93,0,0,0,0,0,0);
}
