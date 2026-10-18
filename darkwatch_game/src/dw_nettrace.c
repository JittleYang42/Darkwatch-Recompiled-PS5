/* Diagnostic: XNet winsock calls from the game's network layer (DW_NETTRACE=1). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_funcs.h"

static int nettrace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DW_NETTRACE") != NULL;
    return on;
}

#define NETWRAP(ADDR) \
    void __real_sub_##ADDR(void); void __wrap_sub_##ADDR(void); \
    void __wrap_sub_##ADDR(void) { \
        static unsigned long n; \
        uint32_t ra = MEM32(g_esp), a[6]; int i; \
        if (!nettrace_on()) { __real_sub_##ADDR(); return; } \
        for (i = 0; i < 6; i++) a[i] = MEM32(g_esp + 4 + 4 * i); \
        __real_sub_##ADDR(); \
        n++; \
        if (n <= 30 || (n % 20000) == 0) \
            fprintf(stderr, "[NET] %s #%lu from %08X (%08X %08X %08X %08X %08X %08X) -> %08X\n", \
                    #ADDR, n, ra, a[0], a[1], a[2], a[3], a[4], a[5], g_eax); \
    }

NETWRAP(0035711B) NETWRAP(0035718A) NETWRAP(00357195) NETWRAP(003571A0)
NETWRAP(003571B6) NETWRAP(003571C1) NETWRAP(003571D0) NETWRAP(003571DB)
NETWRAP(00357200) NETWRAP(00357229) NETWRAP(00357247) NETWRAP(00357154)
NETWRAP(0035717F) NETWRAP(00357163) NETWRAP(00357110) NETWRAP(00357105)

/* Diagnostic: DW_ICALL_SAMPLE=START,SECONDS -- from START seconds after launch,
 * sample the latest indirect-call target for SECONDS and print a histogram:
 * which virtual functions the game is running while it seems stuck. */
#include <pthread.h>
#include <time.h>
#include <unistd.h>
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

static volatile uint32_t *s_main_esp;
static volatile uint32_t s_frame_top;
static uint32_t ch_key[2048][8]; static unsigned ch_cnt[2048];
static void record_chain(uint32_t sp, uint32_t top)
{
    uint32_t all[256], c[8] = {0}, a, n = 0, h, k;
    for (a = sp; a < top && n < 256; a += 4) {
        uint32_t v = MEM32(a);
        if (v >= 0x11000 && v < 0x3423C0) all[n++] = v;
    }
    /* the outermost frames: what the frame is doing, not the leaf */
    for (k = 0; k < 8 && k < n; k++) c[k] = all[n - 1 - k];
    h = 0; for (k = 0; k < 8; k++) h = h * 31 + c[k];
    h = (h * 2654435761u) >> 21;
    while (ch_cnt[h] && memcmp(ch_key[h], c, sizeof c)) h = (h + 1) & 2047;
    memcpy(ch_key[h], c, sizeof c); ch_cnt[h]++;
}
void __real_sub_001E8C26(void); void __wrap_sub_001E8C26(void);
void __wrap_sub_001E8C26(void)
{
    s_main_esp = (volatile uint32_t *)&g_esp;
    s_frame_top = g_esp;
    if (nettrace_on() && MEM32(0x4AD2FCu)) {
        static uint32_t last[12];
        uint32_t o = MEM32(0x4AD2FCu), v[12], k;
        for (k = 0; k < 12; k++) v[k] = MEM32(o + 0xBE4 + 4 * k);
        if (memcmp(v, last, sizeof v)) {
            fprintf(stderr, "[SAVE] obj %08X BE4..C10:", o);
            for (k = 0; k < 12; k++) fprintf(stderr, " %08X", v[k]);
            fprintf(stderr, "\n");
            memcpy(last, v, sizeof v);
        }
    }
    if (nettrace_on()) {
        static unsigned long n;
        if ((n++ % 600) == 0) {
            uint32_t app = g_ecx, node = MEM32(app + 8), k = 0;
            fprintf(stderr, "[APP] #%lu app %08X list:", n, app);
            while (node && k++ < 24) {
                uint32_t obj = MEM32(node);
                fprintf(stderr, " %08X(vt %08X)", obj, obj ? MEM32(obj) : 0);
                node = MEM32(node + 4);
                if (node == MEM32(app + 8)) break;
            }
            fprintf(stderr, "\n");
        }
    }
    __real_sub_001E8C26();
}

static void *icall_sampler(void *arg)
{
    static uint32_t key[4096]; static unsigned cnt[4096];
    const char *s = (const char *)arg;
    unsigned start = 0, secs = 10, i, j, total = 0;
    struct timespec ts = { 0, 300000 };
    sscanf(s, "%u,%u", &start, &secs);
    sleep(start);
    fprintf(stderr, "[ICALL] sampling for %u s\n", secs);
    for (i = 0; i < secs * 3000u; i++) {
        uint32_t t = g_icall_trace[(g_icall_trace_idx - 1) & 15];
        if (s_main_esp && s_frame_top) {
            /* the main thread's guest stack: the first three return addresses */
            uint32_t sp = *s_main_esp, a, n = 0, x = 0;
            for (a = sp; a < s_frame_top && n < 3; a += 4) {
                uint32_t v = MEM32(a);
                if (v >= 0x11000 && v < 0x3423C0) { x = x * 31 + v; n++; if (n == 1) t = v; }
            }
            if (n) { record_chain(sp, s_frame_top); }
        }
        uint32_t h = (t * 2654435761u) >> 20;
        while (key[h] && key[h] != t) h = (h + 1) & 4095;
        key[h] = t; cnt[h]++; total++;
        nanosleep(&ts, NULL);
    }
    for (j = 0; j < 40; j++) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < 4096; i++) if (cnt[i] > best) { best = cnt[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "[ICALL] %08X %5.1f%%\n", key[bi], 100.0 * best / total);
        cnt[bi] = 0;
    }
    for (j = 0; j < 40; j++) {
        unsigned best = 0, bi = 0;
        for (i = 0; i < 2048; i++) if (ch_cnt[i] > best) { best = ch_cnt[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "[STACK] %5.1f%%  outer> %08X %08X %08X %08X %08X %08X %08X %08X\n", 100.0 * best / total,
                ch_key[bi][0], ch_key[bi][1], ch_key[bi][2], ch_key[bi][3],
                ch_key[bi][4], ch_key[bi][5], ch_key[bi][6], ch_key[bi][7]);
        ch_cnt[bi] = 0;
    }
    return NULL;
}

__attribute__((constructor)) static void icall_sampler_start(void)
{
    const char *s = getenv("DW_ICALL_SAMPLE");
    pthread_t t;
    if (s && pthread_create(&t, NULL, icall_sampler, (void *)s) == 0)
        pthread_detach(t);
}

/* Diagnostic: the per-frame game-state tick (DW_NETTRACE). */
void __real_sub_000C5F6D(void); void __wrap_sub_000C5F6D(void);
void __wrap_sub_000C5F6D(void)
{
    static unsigned long n;
    uint32_t o = g_ecx;
    if (nettrace_on() && (n++ % 300) == 0)
        fprintf(stderr, "[STATE] #%lu obj %08X clock+20=%08X +0C=%08X +10=%08X +14=%08X +18=%08X\n", n, o, MEM32(0x4B7AE0u),
                MEM32(o + 0xC), MEM32(o + 0x10), MEM32(o + 0x14), MEM32(o + 0x18));
    __real_sub_000C5F6D();
}

void __real_sub_000C59A4(void); void __wrap_sub_000C59A4(void);
void __wrap_sub_000C59A4(void)
{
    static unsigned long n;
    static uint32_t last[6];
    uint32_t o = g_ecx, s14 = MEM32(o + 0x14), g = MEM32(0x4AC114u);
    uint32_t v[6];
    v[0] = MEM32(o + 8); v[1] = MEM32(o + 0xC); v[2] = MEM32(o + 0x70);
    v[3] = s14 ? MEM32(s14 + 0x70) : 0xDEAD; v[4] = g ? MEM32(g + 0x2C) : 0xDEAD; v[5] = MEM32(o + 0x20);
    n++;
    if (nettrace_on() && memcmp(v, last, sizeof v)) {
        fprintf(stderr, "[MODE] #%lu obj %08X mode=%u st=%u +70=%u sess(%08X)+70=%u g+2C=%u +20=%u\n",
                n, o, v[0], v[1], v[2], s14, v[3], v[4], v[5]);
        memcpy(last, v, sizeof v);
    }
    __real_sub_000C59A4();
}
