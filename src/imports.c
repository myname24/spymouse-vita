/* Host side of libSpyMouse.so's libc/libm/libstdc++/pthread imports (Android bionic ABI -> vitasdk newlib).
 *
 * Differences handled here: softfp float calls (AAPCS wrappers), bionic struct layouts (stat, dirent, timespec, pthread
 * objects that are 4 bytes in bionic), open() flag values, 16-bit wchar_t, stdio's __sF array, ctype tables, atomics, the
 * app's Android storage path. GL imports are in glwrap.c / gen/gen_syms.c. Names listed in imp_syms[] must stay in the form
 *     { "name", (void *)function },
 * because tools/gen.py parses them to decide which imports still need a logging stub.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sched.h>
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include "sm.h"

/* ---------- allocator ----------
 * The game's constructors rely on zeroed memory (Android's allocator hands out fresh zero pages); newlib recycles blocks full of old
 * data (Vita test 3: a render object's array pointer held the ASCII of a freed string). So malloc/realloc zero what they hand out. */
static void *w_malloc(size_t n)
{
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    else { struct mallinfo mi = mallinfo(); sm_log("MALLOC FAILED: %u bytes (heap in use %d KB, arena %d KB)\n", (unsigned)n, mi.uordblks / 1024, mi.arena / 1024); }
    return p;
}
static void *w_realloc(void *p, size_t n)
{
    size_t old = p ? malloc_usable_size(p) : 0;
    void *r = realloc(p, n);
    if (r && n > old) memset((uint8_t *)r + old, 0, n - old);
    return r;
}

/* ---------- paths ---------- */
static const char *remap(const char *p, char *buf, size_t n)
{
    /* The game builds "<storage dir><DATA_FOLDER>/res_960x640/..." where DATA_FOLDER is /Android/data/com.ea.spymouse_row/files
     * (config.properties) and the storage dir is whatever java.c returned, so the Android segment can sit after a Vita prefix
     * ("ux0:data/spymouse/files//Android/data/..."): match it anywhere, not only at the start. */
    static const char APP[] = "/Android/data/com.ea.spymouse_row/files";
    if (!p) return p;
    const char *q = strstr(p, APP);
    if (q) { snprintf(buf, n, DATA_DIR "/files%s", q + sizeof APP - 1); return buf; }
    if (strchr(p, ':') && p[0] != '/') return p;                     /* already a Vita path (ux0:, app0:, ...) */
    if (p[0] == '/') { snprintf(buf, n, DATA_DIR "/root%s", p); return buf; }   /* /proc, /dev, /sdcard ... */
    snprintf(buf, n, DATA_DIR "/files/%s", p);
    return buf;
}
#define RP(p) char _rb[512]; p = remap(p, _rb, sizeof _rb)

/* ---------- stdio ---------- */
static char sF[3][84];                                   /* bionic's __sF: stdin, stdout, stderr (84 bytes each) */
static FILE *F(void *f)
{
    uintptr_t a = (uintptr_t)f, b = (uintptr_t)sF;
    if (a >= b && a < b + sizeof sF) { int i = (int)((a - b) / 84); return i == 0 ? stdin : i == 1 ? stdout : stderr; }
    return f;
}
static volatile int nflog;
#define FLOG(...) do { if (nflog < 1200) { nflog++; sm_log(__VA_ARGS__); } } while (0)
/* trace of the stdio calls of the first REPEATED load (the same file opened twice in a row = the game's retry loop) */
static volatile int trace_on, trace_n;
volatile int sm_trace;                              /* also read by glwrap.c */
static char lastp[300];
static int rep;
#define TR(...) do { if (trace_on && trace_n < 160) { trace_n++; sm_log(__VA_ARGS__); } } while (0)

/* Missing-path cache. The game probes res_960x640/<file> and two res_userdata/cache/... paths before every asset, and each failed
 * lookup is a memory-card access (~1 ms). Remember paths that did not exist; forget them all whenever the game could have created
 * a file (write-mode fopen, O_CREAT open, mkdir, rename). */
#define NEG_N 2048
static uint32_t neg_h[NEG_N]; static int neg_n;
static uint32_t hpath(const char *p) { uint32_t h = 2166136261u; while (*p) h = (h ^ (uint8_t)*p++) * 16777619u; return h | 1; }
static int neg_has(const char *p) { uint32_t h = hpath(p); for (int i = 0; i < neg_n; i++) if (neg_h[i] == h) return 1; return 0; }
static void neg_add(const char *p) { if (neg_n < NEG_N) neg_h[neg_n++] = hpath(p); }
static void neg_flush(void) { neg_n = 0; }

/* I/O counters, printed with the per-300-frames line in main.c */
static volatile uint32_t st_open, st_open_us, st_read_kb, st_read_us, st_neg;
void sm_io_stats(unsigned *v)
{
    v[0] = st_open; v[1] = st_open_us / 1000; v[2] = st_read_kb; v[3] = st_read_us / 1000; v[4] = st_neg;
    st_open = st_open_us = st_read_kb = st_read_us = st_neg = 0;
}

/* Read-only files up to MEMF_MAX are read into RAM with one open/fstat/read/close and served from memory through funopen(). The
 * game opens ~1000 small files per level and seeks to the end and back for each size, and every fseek on a real FILE discards the
 * buffer and goes back to the memory card (Vita test 22: 1.1-2.1 s of reads for 2-4 MB per level load). */
#define MEMF_MAX   (8 * 1024 * 1024)
#define MEMF_TOTAL (48 * 1024 * 1024)
typedef struct { uint8_t *d; long size, pos; } MemF;
static volatile long memf_live;
static int memf_read(void *c, char *b, int n)
{
    MemF *m = c; long left = m->size - m->pos; if (n > left) n = (int)left; if (n <= 0) return 0;
    memcpy(b, m->d + m->pos, (size_t)n); m->pos += n; return n;
}
static fpos_t memf_seek(void *c, fpos_t off, int w)
{
    MemF *m = c; long p = w == SEEK_SET ? (long)off : w == SEEK_CUR ? m->pos + (long)off : m->size + (long)off;
    if (p < 0) return -1;
    if (p > m->size) p = m->size;                        /* reads past the end just return 0 */
    m->pos = p; return (fpos_t)p;
}
static int memf_close(void *c) { MemF *m = c; memf_live -= m->size; free(m->d); free(m); return 0; }
static FILE *memf_open(const char *p)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat s;
    if (fstat(fd, &s) || !S_ISREG(s.st_mode) || s.st_size <= 0 || s.st_size > MEMF_MAX || memf_live + s.st_size > MEMF_TOTAL) { close(fd); return (FILE *)-1; }
    MemF *m = malloc(sizeof *m); uint8_t *d = malloc((size_t)s.st_size);
    if (!m || !d) { free(m); free(d); close(fd); return (FILE *)-1; }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    long got = 0;
    while (got < s.st_size) { int r = read(fd, d + got, (size_t)(s.st_size - got)); if (r <= 0) break; got += r; }
    st_read_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0); st_read_kb += (uint32_t)(got / 1024);
    close(fd);
    if (got != s.st_size) { free(m); free(d); return (FILE *)-1; }
    m->d = d; m->size = got; m->pos = 0;
    FILE *f = funopen(m, memf_read, NULL, memf_seek, memf_close);
    if (!f) { free(m); free(d); return (FILE *)-1; }
    setvbuf(f, NULL, _IONBF, 0);                         /* the data is already in RAM */
    memf_live += got;
    return f;
}

static FILE *w_fopen(const char *p, const char *m)
{
    RP(p);
    int rd = m[0] == 'r' && !strchr(m, '+');
    if (!rd) neg_flush();
    else if (neg_has(p)) { errno = ENOENT; st_neg++; return NULL; }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    FILE *f = NULL;
    int cached = 0;
    if (rd) {
        errno = 0;
        f = memf_open(p);                                /* NULL: could not open, (FILE *)-1: too big / failed, use stdio */
        if (f == (FILE *)-1) f = NULL; else cached = 1;
    }
    if (!f && !(cached && errno == ENOENT)) {
        f = fopen(p, m);
        if (f && m[0] == 'r') setvbuf(f, NULL, _IOFBF, 128 * 1024);     /* newlib's 1 KB default means one memory-card read per KB */
    }
    st_open++; st_open_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    if (!f && rd) neg_add(p);
    if (!sm_verbose) { /* quiet mode: no size probing or logging */ }
    else if (f && m[0] == 'r' && nflog < 1200) {         /* log the size too: .gui reads returned 0 bytes (Vita test 11) */
        long cur = ftell(f); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, cur, SEEK_SET);
        FLOG("file: fopen %s [%s] -> ok, %ld bytes\n", p, m, sz);
    } else if (f || !strstr(p, "res_userdata/cache")) FLOG("file: fopen %s [%s] -> %s\n", p, m, f ? "ok" : "FAIL");
    if (f && !trace_n) {
        if (!strcmp(lastp, p)) {
            if (++rep == 2) { trace_on = 1; sm_trace = 1; sm_log("tr: repeated load of %s\n", p); }
        } else { rep = 0; snprintf(lastp, sizeof lastp, "%s", p); }
    }
    return f;
}

/* the game's own log (libpng errors, asset problems, ...) */
static volatile int nalog;
static int w_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (nalog < 400) { nalog++; sm_log("[droid %d %s] %s\n", prio, tag ? tag : "", b); }
    return 0;
}
static int w_android_log_vprint(int prio, const char *tag, const char *fmt, va_list a)
{
    char b[400]; vsnprintf(b, sizeof b, fmt, a);
    if (nalog < 400) { nalog++; sm_log("[droid %d %s] %s\n", prio, tag ? tag : "", b); }
    return 0;
}
static int w_android_log_write(int prio, const char *tag, const char *text)
{
    if (nalog < 400) { nalog++; sm_log("[droid %d %s] %s\n", prio, tag ? tag : "", text ? text : ""); }
    return 0;
}
static int w_fclose(void *f) { int r = fclose(F(f)); TR("tr: fclose %p -> %d\n", f, r); return r; }
static size_t w_fread(void *b, size_t s, size_t n, void *f)
{
    FILE *h = F(f);
    long before = (sm_verbose && h != stdin) ? ftell(h) : 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t r = fread(b, s, n, h);
    st_read_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0); st_read_kb += (uint32_t)(s * r / 1024);
    TR("tr: fread %p size %u x %u -> %u\n", f, (unsigned)s, (unsigned)n, (unsigned)r);
    static volatile int nz;
    if (r == 0 && s * n > 0 && nz < 40) { nz++; sm_log("fread returned 0: %u x %u at pos %ld, feof=%d ferror=%d errno=%d\n", (unsigned)s, (unsigned)n, before, feof(h), ferror(h), errno); }
    return r;
}
static size_t w_fwrite(const void *b, size_t s, size_t n, void *f) { return fwrite(b, s, n, F(f)); }
static int w_fseek(void *f, long o, int w) { int r = fseek(F(f), o, w); TR("tr: fseek %p %ld %d -> %d\n", f, o, w, r); return r; }
static long w_ftell(void *f) { long r = ftell(F(f)); TR("tr: ftell %p -> %ld\n", f, r); return r; }
static int w_fflush(void *f) { return f ? fflush(F(f)) : fflush(NULL); }
static int w_fgetc(void *f) { return fgetc(F(f)); }
static char *w_fgets(char *s, int n, void *f) { return fgets(s, n, F(f)); }
static int w_fputc(int c, void *f) { return fputc(c, F(f)); }
static int w_fputs(const char *s, void *f) { return fputs(s, F(f)); }
static int w_ungetc(int c, void *f) { return ungetc(c, F(f)); }
static int w_fprintf(void *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf(F(f), fmt, a); va_end(a); return r; }
static int w_putchar(int c) { return putchar(c); }
static int w_fwide(void *f, int m) { (void)f; (void)m; return 0; }

/* ---------- files (bionic ABI) ---------- */
struct bstat {                                           /* bionic ARM struct stat, 104 bytes */
    uint64_t st_dev; uint8_t pad0[4]; uint32_t ino32; uint32_t st_mode, st_nlink, st_uid, st_gid; uint64_t st_rdev;
    uint8_t pad3[4]; int64_t st_size; uint32_t st_blksize; uint64_t st_blocks;
    uint32_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint64_t st_ino;
};
_Static_assert(sizeof(struct bstat) == 104 && offsetof(struct bstat, st_size) == 48, "bionic stat layout");
static void fill_stat(struct bstat *o, const struct stat *s)
{
    memset(o, 0, sizeof *o);
    o->st_mode = s->st_mode; o->st_size = s->st_size; o->st_nlink = 1; o->st_blksize = 4096;
    o->st_blocks = (s->st_size + 511) / 512; o->st_ino = o->ino32 = (uint32_t)s->st_ino;
    o->atime = (uint32_t)s->st_atime; o->mtime = (uint32_t)s->st_mtime; o->ctime = (uint32_t)s->st_ctime;
}
static int w_stat(const char *p, struct bstat *o)
{
    RP(p); struct stat s;
    if (neg_has(p)) { errno = ENOENT; st_neg++; return -1; }
    int r = stat(p, &s);
    if (!r) fill_stat(o, &s); else neg_add(p);
    return r;
}
static int w_fstat(int fd, struct bstat *o) { struct stat s; int r = fstat(fd, &s); if (!r) fill_stat(o, &s); return r; }
static int w_open(const char *p, int fl, int mode)
{
    RP(p);
    int h = fl & 3;                                      /* O_RDONLY/WRONLY/RDWR are 0/1/2 on both */
    if (fl & 0x40) h |= O_CREAT;  if (fl & 0x80) h |= O_EXCL;   if (fl & 0x200) h |= O_TRUNC;  if (fl & 0x400) h |= O_APPEND;
    int ro = (fl & 3) == 0 && !(fl & 0x40);
    if (!ro) neg_flush();
    else if (neg_has(p)) { errno = ENOENT; st_neg++; return -1; }
    int r = open(p, h, mode);
    if (r < 0 && ro) neg_add(p);
    if (sm_verbose) FLOG("file: open %s [fl %#x] -> %d\n", p, fl, r);
    return r;
}
static int w_lseek(int fd, int off, int wh) { return (int)lseek(fd, off, wh); }
static int w_mkdir(const char *p, int m) { RP(p); neg_flush(); return mkdir(p, m); }
static int w_rmdir(const char *p) { RP(p); return rmdir(p); }
static int w_unlink(const char *p) { RP(p); return unlink(p); }
static int w_remove(const char *p) { RP(p); return remove(p); }
static int w_rename(const char *a, const char *b) { char x[512], y[512]; neg_flush(); return rename(remap(a, x, sizeof x), remap(b, y, sizeof y)); }
static int w_chdir(const char *p) { (void)p; return 0; }
static char *w_getcwd(char *b, size_t n) { if (!b) return NULL; snprintf(b, n, "%s", DATA_DIR "/files"); return b; }
static int w_statfs(const char *p, uint32_t *o)         /* bionic struct statfs: type, bsize, blocks(u64), bfree(u64), bavail(u64) ... */
{ (void)p; memset(o, 0, 64); o[1] = 4096; o[2] = 1u << 20; o[4] = 1u << 19; o[6] = 1u << 19; return 0; }

struct gdirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };   /* d_name at 19 */
static DIR *w_opendir(const char *p) { RP(p); DIR *d = opendir(p); FLOG("file: opendir %s -> %s\n", p, d ? "ok" : "FAIL"); return d; }
static struct gdirent *w_readdir(DIR *d)
{
    static struct gdirent g;
    struct dirent *e = readdir(d);
    if (!e) return NULL;
    memset(&g, 0, sizeof g);
    g.d_ino = 1; g.d_reclen = sizeof g; g.d_type = 0;
    strncpy(g.d_name, e->d_name, 255);
    return &g;
}
static int w_readdir_r(DIR *d, struct gdirent *ent, struct gdirent **res)
{
    struct gdirent *g = w_readdir(d);
    if (!g) { *res = NULL; return 0; }
    *ent = *g; *res = ent;
    return 0;
}
static int w_closedir(DIR *d) { return closedir(d); }

/* ---------- time ---------- */
struct gts { int32_t sec, nsec; };
static int w_clock_gettime(int clk, struct gts *t)
{
    uint64_t us = clk == 0 ? (uint64_t)sceKernelGetSystemTimeWide() : (uint64_t)sceKernelGetProcessTimeWide();
    if (clk == 0) us += 1700000000ull * 1000000ull;      /* a plausible wall clock */
    t->sec = (int32_t)(us / 1000000); t->nsec = (int32_t)(us % 1000000) * 1000;
    { static volatile int n; if (n < 60 && sceKernelGetProcessTimeLow() > 10400000u) { n++; sm_log("audio: clock_gettime(%d) = %d.%09d thread %x lr=0x%x\n", clk, t->sec, t->nsec, sceKernelGetThreadId(), (unsigned)__builtin_return_address(0) - (unsigned)SO_BASE); } }
    return 0;
}
static int w_gettimeofday(int32_t *tv, void *tz)
{
    (void)tz; uint64_t us = sceKernelGetSystemTimeWide() + 1700000000ull * 1000000ull;
    tv[0] = (int32_t)(us / 1000000); tv[1] = (int32_t)(us % 1000000); return 0;
}
static int32_t w_time(int32_t *t) { int32_t v = (int32_t)(sceKernelGetSystemTimeWide() / 1000000 + 1700000000u); if (t) *t = v; return v; }
static int w_nanosleep(const struct gts *t, struct gts *rem)
{
    (void)rem; uint32_t us = (uint32_t)t->sec * 1000000u + (uint32_t)t->nsec / 1000u;
    static volatile int n; if (n < 80 && sceKernelGetProcessTimeLow() > 11000000u) { n++; sm_log("audio: nanosleep %u us (thread %x) lr=0x%x\n", us, sceKernelGetThreadId(), (unsigned)__builtin_return_address(0) - (unsigned)SO_BASE); }
    sceKernelDelayThread(us); return 0;
}
static struct tm *w_localtime(const int32_t *t) { time_t v = *t; return gmtime(&v); }
static struct tm *w_gmtime(const int32_t *t) { time_t v = *t; return gmtime(&v); }
static int32_t w_clock(void) { return (int32_t)sceKernelGetProcessTimeLow(); }
static char *tzn[2] = { "UTC", "UTC" };

/* ---------- float ABI wrappers ---------- */
#define F1(n)  AAPCS static float  wf_##n(float x) { return n(x); }
#define F2(n)  AAPCS static float  wf_##n(float x, float y) { return n(x, y); }
#define D1(n)  AAPCS static double wd_##n(double x) { return n(x); }
#define D2(n)  AAPCS static double wd_##n(double x, double y) { return n(x, y); }
F1(sinf) F1(cosf) F1(tanf) F1(sqrtf) F1(floorf) F1(ceilf) F1(log10f) F2(atan2f)
D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(exp) D1(log) D1(log10) D1(sqrt) D1(floor) D1(ceil)
D2(atan2) D2(pow) D2(fmod)
AAPCS static double wd_modf(double x, double *ip) { return modf(x, ip); }
AAPCS static double wd_frexp(double x, int *e) { return frexp(x, e); }
AAPCS static double wd_ldexp(double x, int e) { return ldexp(x, e); }
AAPCS static double wd_strtod(const char *s, char **e) { return strtod(s, e); }
AAPCS static int wd_isinf(double x) { return isinf(x) ? (x > 0 ? 1 : -1) : 0; }

/* ---------- bionic data ---------- */
static uint8_t  bctype[257];
static int16_t  btolower[257], btoupper[257];
/* bionic (Gingerbread) declares these as pointer variables: "extern const short *_tolower_tab_;" (the game loads the GOT entry,
 * then dereferences it, then indexes) */
static const uint8_t *p_ctype = bctype;
static const int16_t *p_tolower = btolower, *p_toupper = btoupper;
static uint32_t stack_chk_guard = 0x5a17c0de;
static int      page_size = 4096;
static int      dso_handle;
static void init_ctype(void)
{
    for (int c = 0; c < 256; c++) {
        uint8_t f = 0;
        if (c >= 'A' && c <= 'Z') f |= 0x01; if (c >= 'a' && c <= 'z') f |= 0x02; if (c >= '0' && c <= '9') f |= 0x04;
        if ((c >= 9 && c <= 13) || c == ' ') f |= 0x08;
        if (c > 32 && c < 127 && !(f & 7)) f |= 0x10;
        if (c < 32 || c == 127) f |= 0x20;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x40;
        if (c == ' ') f |= 0x80;
        bctype[c + 1] = f;
        btolower[c + 1] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        btoupper[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    btolower[0] = btoupper[0] = -1;
}
static int *w_errno(void) { return &errno; }
static void w_stack_chk_fail(void) { sm_log("stack smashing detected\n"); abort(); }
static void w_exit(int c) { sm_log("guest exit(%d)\n", c); sceKernelExitProcess(c); }
static void w_abort(void) { sm_log("guest abort()\n"); abort(); }

/* ---------- atomics (bionic <sys/atomics.h>) ---------- */
static int a_cmpxchg(int o, int n, volatile int *p) { return !__sync_bool_compare_and_swap(p, o, n); }
static int a_inc(volatile int *p) { return __sync_fetch_and_add(p, 1); }
static int a_dec(volatile int *p) { return __sync_fetch_and_sub(p, 1); }
static int a_swap(int n, volatile int *p) { return __sync_lock_test_and_set(p, n); }

/* ---------- C++ runtime bits ---------- */
static pthread_mutex_t guard_mx = PTHREAD_MUTEX_INITIALIZER;
static int cxa_guard_acquire(volatile uint8_t *g) { pthread_mutex_lock(&guard_mx); if (*g) { pthread_mutex_unlock(&guard_mx); return 0; } return 1; }
static void cxa_guard_release(volatile uint8_t *g) { *g = 1; pthread_mutex_unlock(&guard_mx); }
static void cxa_pure_virtual(void) { sm_log("pure virtual call\n"); abort(); }
static int aeabi_atexit(void *o, void (*d)(void *), void *h) { (void)o; (void)d; (void)h; return 0; }
/* ARM EHABI: tell the guest's unwinder where its exception index lives */
static void *w_find_exidx(uint32_t pc, int *count)
{
    if (pc >= g_so.base && pc < g_so.base + g_so.span && g_so.exidx) { *count = (int)(g_so.exidx_size / 8); return (void *)(uintptr_t)g_so.exidx; }
    *count = 0; return NULL;
}
extern void __cxa_begin_cleanup(void), __cxa_call_unexpected(void), __cxa_type_match(void);

/* ---------- pthreads (bionic objects are 4 bytes; static initialisers are 0 / 0x4000 / 0x8000) ---------- */
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint32_t flags; void *stack_base; uint32_t stack_size, guard, policy; int32_t prio; } gattr;
static pthread_mutex_t *gm(uint32_t *m)
{
    if (*m >= 0x10000) return (pthread_mutex_t *)(uintptr_t)*m;
    pthread_mutex_lock(&big);
    if (*m < 0x10000) {
        uint32_t kind = *m & 0xC000;
        pthread_mutexattr_t a; pthread_mutexattr_init(&a);
        if (kind == 0x4000) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_t *h = malloc(sizeof *h); pthread_mutex_init(h, &a); *m = (uint32_t)(uintptr_t)h;
    }
    pthread_mutex_unlock(&big);
    return (pthread_mutex_t *)(uintptr_t)*m;
}
static int w_mutex_init(uint32_t *m, const int *attr)
{
    *m = (attr && *attr == 1) ? 0x4000 : 0;
    gm(m);
    return 0;
}
static int w_mutex_lock(uint32_t *m) { return pthread_mutex_lock(gm(m)); }
static int w_mutex_unlock(uint32_t *m) { return pthread_mutex_unlock(gm(m)); }
static int w_mutex_trylock(uint32_t *m) { return pthread_mutex_trylock(gm(m)); }
static int w_mutex_destroy(uint32_t *m) { if (*m >= 0x10000) { pthread_mutex_destroy((pthread_mutex_t *)(uintptr_t)*m); free((void *)(uintptr_t)*m); *m = 0; } return 0; }
static int w_mutexattr_init(int *a) { *a = 0; return 0; }
static int w_mutexattr_destroy(int *a) { (void)a; return 0; }
static int w_mutexattr_settype(int *a, int t) { *a = t; return 0; }
static int w_mutexattr_setpshared(int *a, int s) { (void)a; (void)s; return 0; }

static pthread_cond_t *gc(uint32_t *c)
{
    if (*c >= 0x10000) return (pthread_cond_t *)(uintptr_t)*c;
    pthread_mutex_lock(&big);
    if (*c < 0x10000) { pthread_cond_t *h = malloc(sizeof *h); pthread_cond_init(h, NULL); *c = (uint32_t)(uintptr_t)h; }
    pthread_mutex_unlock(&big);
    return (pthread_cond_t *)(uintptr_t)*c;
}
static int w_cond_init(uint32_t *c, const void *a) { (void)a; *c = 0; gc(c); return 0; }
static int w_cond_destroy(uint32_t *c) { if (*c >= 0x10000) { pthread_cond_destroy((pthread_cond_t *)(uintptr_t)*c); free((void *)(uintptr_t)*c); *c = 0; } return 0; }
static int w_cond_signal(uint32_t *c) { return pthread_cond_signal(gc(c)); }
static int w_cond_broadcast(uint32_t *c) { return pthread_cond_broadcast(gc(c)); }
static int w_cond_wait(uint32_t *c, uint32_t *m) { return pthread_cond_wait(gc(c), gm(m)); }
/* the guest's absolute deadline is on our fake CLOCK_REALTIME (w_clock_gettime(0)); rebase it onto the host clock */
static void rebase_deadline(const struct gts *t, struct timespec *ts)
{
    struct gts now; w_clock_gettime(0, &now);
    int64_t rem = ((int64_t)t->sec - now.sec) * 1000000000ll + ((int64_t)t->nsec - now.nsec);
    if (rem < 0) rem = 0;
    clock_gettime(CLOCK_REALTIME, ts);
    int64_t ns = ts->tv_nsec + rem % 1000000000ll;
    ts->tv_sec += rem / 1000000000ll + ns / 1000000000ll; ts->tv_nsec = ns % 1000000000ll;
}
static int w_cond_timedwait(uint32_t *c, uint32_t *m, const struct gts *t)
{
    struct timespec ts; rebase_deadline(t, &ts);
    int r = pthread_cond_timedwait(gc(c), gm(m), &ts);
    static volatile int n; if (n < 60 && sceKernelGetProcessTimeLow() > 11000000u) { n++; sm_log("audio: cond_timedwait -> %d (thread %x)\n", r, sceKernelGetThreadId()); }
    return r;
}

static int w_attr_init(gattr *a) { memset(a, 0, sizeof *a); a->stack_size = 1024 * 1024; return 0; }       /* bionic's default is 1 MB */
static int w_attr_destroy(gattr *a) { (void)a; return 0; }
static int w_attr_setdetachstate(gattr *a, int s) { a->flags = (a->flags & ~1u) | (s ? 1u : 0); return 0; }
static int w_attr_setstacksize(gattr *a, uint32_t s) { a->stack_size = s; return 0; }
static int w_attr_setstack(gattr *a, void *b, uint32_t s) { a->stack_base = b; a->stack_size = s; return 0; }
static int w_attr_getstack(const gattr *a, void **b, uint32_t *s) { *b = a->stack_base; *s = a->stack_size; return 0; }
static int w_attr_setschedparam(gattr *a, const int *p) { a->prio = *p; return 0; }
static int w_getattr_np(pthread_t t, gattr *a) { (void)t; w_attr_init(a); return 0; }
static int w_setschedparam(pthread_t t, int pol, const int *p) { (void)t; (void)pol; (void)p; return 0; }
static int w_getschedparam(pthread_t t, int *pol, int *p) { (void)t; *pol = 0; *p = 0; return 0; }
static int w_pthread_create(pthread_t *t, const gattr *a, void *(*fn)(void *), void *arg)
{
    pthread_attr_t h; pthread_attr_init(&h);
    /* never less than 1 MB: the game asked for 256 KB and overflowed it while decoding PNGs (Vita test 9) */
    uint32_t ss = a && a->stack_size > 1024 * 1024 ? a->stack_size : 1024 * 1024;
    pthread_attr_setstacksize(&h, ss);
    if (a && (a->flags & 1)) pthread_attr_setdetachstate(&h, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(t, &h, fn, arg);
    sm_log("pthread_create fn=%p stack=%u -> %d\n", fn, (unsigned)ss, r);
    return r;
}
static int w_pthread_once(volatile int *o, void (*fn)(void))
{
    pthread_mutex_lock(&big);
    int run = !*o; *o = 1;
    pthread_mutex_unlock(&big);
    if (run) fn();
    return 0;
}

/* ---------- semaphores (bionic sem_t holds a count; ours stores a host handle) ---------- */
static sem_t *gs(uint32_t *s) { return (sem_t *)(uintptr_t)*s; }
static int w_sem_init(uint32_t *s, int sh, unsigned v) { (void)sh; sem_t *h = malloc(sizeof *h); sem_init(h, 0, v); *s = (uint32_t)(uintptr_t)h; return 0; }
static int w_sem_destroy(uint32_t *s) { sem_destroy(gs(s)); free(gs(s)); *s = 0; return 0; }
static int w_sem_post(uint32_t *s) { return sem_post(gs(s)); }
static int w_sem_wait(uint32_t *s) { return sem_wait(gs(s)); }
static int w_sem_trywait(uint32_t *s) { return sem_trywait(gs(s)); }
static int w_sem_getvalue(uint32_t *s, int *v) { return sem_getvalue(gs(s), v); }
static int w_sem_timedwait(uint32_t *s, const struct gts *t)
{
    struct timespec ts; rebase_deadline(t, &ts);
    int r = sem_timedwait(gs(s), &ts);
    static volatile int n; if (n < 60 && sceKernelGetProcessTimeLow() > 11000000u) { n++; sm_log("audio: sem_timedwait -> %d (thread %x)\n", r, sceKernelGetThreadId()); }
    return r;
}

/* ---------- misc ---------- */
static long w_sysconf(int n)
{
    switch (n) { case 0x27: return 4096; case 0x60: case 0x61: return 4; case 0x62: return 65536; case 0x63: return 32768; }
    return -1;
}
static void *w_mmap(void *a, size_t len, int prot, int fl, int fd, long off)
{
    (void)a; (void)prot; (void)fd; (void)off;
    if (!(fl & 0x20)) { sm_log("mmap of a file is not supported\n"); return (void *)-1; }
    void *p = memalign(4096, len);
    if (!p) return (void *)-1;
    memset(p, 0, len);
    return p;
}
static int w_munmap(void *a, size_t l) { (void)l; free(a); return 0; }
static void *w_sbrk(int n) { (void)n; return (void *)-1; }
static char *w_getenv(const char *n) { (void)n; return NULL; }
static int w_setenv(const char *a, const char *b, int c) { (void)a; (void)b; (void)c; return 0; }
static int w_zero(void) { return 0; }
static int w_neg1(void) { return -1; }
static void *w_null(void) { return NULL; }
static int w_ftruncate(int fd, long len) { (void)fd; (void)len; return -1; }
static int w_fsync(int fd) { (void)fd; return 0; }

/* wchar_t is 16 bit in this ABI */
static int w_wcscmp(const uint16_t *a, const uint16_t *b) { while (*a && *a == *b) a++, b++; return (int)*a - (int)*b; }
static uint16_t *w_wcscpy(uint16_t *d, const uint16_t *s) { uint16_t *r = d; while ((*d++ = *s++)); return r; }
static size_t w_wcslen(const uint16_t *s) { size_t n = 0; while (s[n]) n++; return n; }
static uint16_t *w_wcsncpy(uint16_t *d, const uint16_t *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static uint16_t *w_wcsstr(const uint16_t *h, const uint16_t *nd)
{
    size_t n = w_wcslen(nd);
    if (!n) return (uint16_t *)h;
    for (; *h; h++) {
        size_t i = 0;
        while (i < n && h[i] == nd[i]) i++;
        if (i == n) return (uint16_t *)h;
        if (!h[i]) break;
    }
    return NULL;
}

void sm_imports_init(void) { init_ctype(); }

const ImpSym imp_syms[] = {
    /* C++ runtime */
    { "__cxa_guard_acquire", (void *)cxa_guard_acquire },
    { "__cxa_guard_release", (void *)cxa_guard_release },
    { "__cxa_pure_virtual", (void *)cxa_pure_virtual },
    { "__aeabi_atexit", (void *)aeabi_atexit },
    { "__cxa_begin_cleanup", (void *)__cxa_begin_cleanup },
    { "__cxa_call_unexpected", (void *)__cxa_call_unexpected },
    { "__cxa_type_match", (void *)__cxa_type_match },
    { "__gnu_Unwind_Find_exidx", (void *)w_find_exidx },
    { "__dso_handle", (void *)&dso_handle },
    /* bionic data and helpers */
    { "__sF", (void *)sF },
    { "_ctype_", (void *)&p_ctype },
    { "_tolower_tab_", (void *)&p_tolower },
    { "_toupper_tab_", (void *)&p_toupper },
    { "__stack_chk_guard", (void *)&stack_chk_guard },
    { "__stack_chk_fail", (void *)w_stack_chk_fail },
    { "__page_size", (void *)&page_size },
    { "__errno", (void *)w_errno },
    { "__isinf", (void *)wd_isinf },
    { "__atomic_cmpxchg", (void *)a_cmpxchg },
    { "__atomic_inc", (void *)a_inc },
    { "__atomic_dec", (void *)a_dec },
    { "__atomic_swap", (void *)a_swap },
    { "tzname", (void *)tzn },
    /* logging */
    { "__android_log_print", (void *)w_android_log_print },
    { "__android_log_vprint", (void *)w_android_log_vprint },
    { "__android_log_write", (void *)w_android_log_write },
    /* memory / string / conversion */
    { "malloc", (void *)w_malloc }, { "calloc", (void *)calloc }, { "realloc", (void *)w_realloc }, { "free", (void *)free },
    { "memcpy", (void *)memcpy }, { "memmove", (void *)memmove }, { "memset", (void *)memset }, { "memcmp", (void *)memcmp },
    { "bcopy", (void *)memmove },
    { "strlen", (void *)strlen }, { "strcmp", (void *)strcmp }, { "strncmp", (void *)strncmp }, { "strcpy", (void *)strcpy },
    { "strncpy", (void *)strncpy }, { "strcat", (void *)strcat }, { "strchr", (void *)strchr }, { "strrchr", (void *)strrchr },
    { "strstr", (void *)strstr }, { "atoi", (void *)atoi }, { "strtol", (void *)strtol }, { "strtod", (void *)wd_strtod },
    { "qsort", (void *)qsort }, { "abort", (void *)w_abort }, { "exit", (void *)w_exit },
    { "snprintf", (void *)snprintf }, { "sprintf", (void *)sprintf }, { "vsnprintf", (void *)vsnprintf },
    { "vsprintf", (void *)vsprintf }, { "sscanf", (void *)sscanf },
    { "srand48", (void *)srand48 }, { "lrand48", (void *)lrand48 },
    { "setjmp", (void *)setjmp }, { "longjmp", (void *)longjmp },
    { "wcscmp", (void *)w_wcscmp }, { "wcscpy", (void *)w_wcscpy }, { "wcslen", (void *)w_wcslen },
    { "wcsncpy", (void *)w_wcsncpy }, { "wcsstr", (void *)w_wcsstr },
    /* math */
    { "sinf", (void *)wf_sinf }, { "cosf", (void *)wf_cosf }, { "tanf", (void *)wf_tanf }, { "sqrtf", (void *)wf_sqrtf },
    { "floorf", (void *)wf_floorf }, { "ceilf", (void *)wf_ceilf }, { "log10f", (void *)wf_log10f }, { "atan2f", (void *)wf_atan2f },
    { "sin", (void *)wd_sin }, { "cos", (void *)wd_cos }, { "tan", (void *)wd_tan }, { "asin", (void *)wd_asin },
    { "acos", (void *)wd_acos }, { "atan", (void *)wd_atan }, { "exp", (void *)wd_exp }, { "log", (void *)wd_log },
    { "log10", (void *)wd_log10 }, { "sqrt", (void *)wd_sqrt }, { "floor", (void *)wd_floor }, { "ceil", (void *)wd_ceil },
    { "atan2", (void *)wd_atan2 }, { "pow", (void *)wd_pow }, { "fmod", (void *)wd_fmod }, { "modf", (void *)wd_modf },
    { "frexp", (void *)wd_frexp }, { "ldexp", (void *)wd_ldexp },
    /* stdio */
    { "fopen", (void *)w_fopen }, { "fclose", (void *)w_fclose }, { "fread", (void *)w_fread }, { "fwrite", (void *)w_fwrite },
    { "fseek", (void *)w_fseek }, { "ftell", (void *)w_ftell }, { "fflush", (void *)w_fflush }, { "fgetc", (void *)w_fgetc },
    { "fgets", (void *)w_fgets }, { "fputc", (void *)w_fputc }, { "fputs", (void *)w_fputs }, { "ungetc", (void *)w_ungetc },
    { "fprintf", (void *)w_fprintf }, { "putchar", (void *)w_putchar }, { "fwide", (void *)w_fwide },
    /* files */
    { "open", (void *)w_open }, { "close", (void *)close }, { "read", (void *)read }, { "write", (void *)write },
    { "lseek", (void *)w_lseek }, { "stat", (void *)w_stat }, { "fstat", (void *)w_fstat }, { "statfs", (void *)w_statfs },
    { "mkdir", (void *)w_mkdir }, { "rmdir", (void *)w_rmdir }, { "unlink", (void *)w_unlink }, { "remove", (void *)w_remove },
    { "rename", (void *)w_rename }, { "chdir", (void *)w_chdir }, { "getcwd", (void *)w_getcwd },
    { "opendir", (void *)w_opendir }, { "readdir", (void *)w_readdir }, { "readdir_r", (void *)w_readdir_r },
    { "closedir", (void *)w_closedir }, { "ftruncate", (void *)w_ftruncate }, { "fsync", (void *)w_fsync },
    { "chmod", (void *)w_zero }, { "utime", (void *)w_zero }, { "fcntl", (void *)w_zero },
    { "ioctl", (void *)w_neg1 }, { "poll", (void *)w_zero },
    /* time */
    { "clock_gettime", (void *)w_clock_gettime }, { "gettimeofday", (void *)w_gettimeofday }, { "time", (void *)w_time },
    { "nanosleep", (void *)w_nanosleep }, { "localtime", (void *)w_localtime }, { "gmtime", (void *)w_gmtime },
    { "clock", (void *)w_clock }, { "strftime", (void *)strftime }, { "asctime", (void *)asctime },
    /* process / system */
    { "sysconf", (void *)w_sysconf }, { "mmap", (void *)w_mmap }, { "munmap", (void *)w_munmap }, { "sbrk", (void *)w_sbrk },
    { "getenv", (void *)w_getenv }, { "setenv", (void *)w_setenv }, { "prctl", (void *)w_zero },
    { "sched_yield", (void *)sched_yield }, { "raise", (void *)w_zero },
    { "fork", (void *)w_neg1 }, { "execv", (void *)w_neg1 }, { "system", (void *)w_neg1 }, { "waitpid", (void *)w_neg1 },
    /* sockets: no network on this port */
    { "socket", (void *)w_neg1 }, { "connect", (void *)w_neg1 }, { "bind", (void *)w_neg1 }, { "listen", (void *)w_neg1 },
    { "accept", (void *)w_neg1 }, { "send", (void *)w_neg1 }, { "sendto", (void *)w_neg1 }, { "recv", (void *)w_neg1 },
    { "recvfrom", (void *)w_neg1 }, { "shutdown", (void *)w_neg1 }, { "setsockopt", (void *)w_neg1 },
    { "getsockopt", (void *)w_neg1 }, { "getsockname", (void *)w_neg1 }, { "getpeername", (void *)w_neg1 },
    { "gethostname", (void *)w_neg1 }, { "gethostbyname", (void *)w_null }, { "inet_addr", (void *)w_neg1 },
    { "inet_ntoa", (void *)w_null },
    /* pthread */
    { "pthread_create", (void *)w_pthread_create }, { "pthread_exit", (void *)pthread_exit }, { "pthread_self", (void *)pthread_self },
    { "pthread_once", (void *)w_pthread_once },
    { "pthread_key_create", (void *)pthread_key_create }, { "pthread_key_delete", (void *)pthread_key_delete },
    { "pthread_getspecific", (void *)pthread_getspecific }, { "pthread_setspecific", (void *)pthread_setspecific },
    { "pthread_mutex_init", (void *)w_mutex_init }, { "pthread_mutex_destroy", (void *)w_mutex_destroy },
    { "pthread_mutex_lock", (void *)w_mutex_lock }, { "pthread_mutex_unlock", (void *)w_mutex_unlock },
    { "pthread_mutex_trylock", (void *)w_mutex_trylock },
    { "pthread_mutexattr_init", (void *)w_mutexattr_init }, { "pthread_mutexattr_destroy", (void *)w_mutexattr_destroy },
    { "pthread_mutexattr_settype", (void *)w_mutexattr_settype }, { "pthread_mutexattr_setpshared", (void *)w_mutexattr_setpshared },
    { "pthread_cond_init", (void *)w_cond_init }, { "pthread_cond_destroy", (void *)w_cond_destroy },
    { "pthread_cond_signal", (void *)w_cond_signal }, { "pthread_cond_broadcast", (void *)w_cond_broadcast },
    { "pthread_cond_wait", (void *)w_cond_wait }, { "pthread_cond_timedwait", (void *)w_cond_timedwait },
    { "pthread_attr_init", (void *)w_attr_init }, { "pthread_attr_destroy", (void *)w_attr_destroy },
    { "pthread_attr_setdetachstate", (void *)w_attr_setdetachstate }, { "pthread_attr_setstacksize", (void *)w_attr_setstacksize },
    { "pthread_attr_setstack", (void *)w_attr_setstack }, { "pthread_attr_getstack", (void *)w_attr_getstack },
    { "pthread_attr_setschedparam", (void *)w_attr_setschedparam }, { "pthread_getattr_np", (void *)w_getattr_np },
    { "pthread_setschedparam", (void *)w_setschedparam }, { "pthread_getschedparam", (void *)w_getschedparam },
    { "sem_init", (void *)w_sem_init }, { "sem_destroy", (void *)w_sem_destroy }, { "sem_post", (void *)w_sem_post },
    { "sem_wait", (void *)w_sem_wait }, { "sem_trywait", (void *)w_sem_trywait }, { "sem_getvalue", (void *)w_sem_getvalue },
    { "sem_timedwait", (void *)w_sem_timedwait },
    { NULL, NULL }
};

void *sm_resolve(const char *name)
{
    for (const ImpSym *s = imp_syms; s->name; s++) if (!strcmp(s->name, name)) return s->addr;
    for (const ImpSym *s = gen_syms; s->name; s++) if (!strcmp(s->name, name)) return s->addr;
    return NULL;
}
