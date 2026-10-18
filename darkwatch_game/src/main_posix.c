/*
 * Darkwatch (USA, CC-022) - static recompilation, POSIX host.
 *
 * The Linux (and, later, PS5/FreeBSD) counterpart of the template's WinMain.
 * Same boot order: load the XBE, map guest memory, kernel, paths, kernel
 * bridge, guest stack, dispatch table, then the recompiled entry point.
 *
 * XBE (from xbe_parser / darkwatch_recomp/analysis/xbe_report.md):
 *   Title:          Darkwatch, CC-022 (0x43430016), XDK 1.0.5849, retail
 *   Base / image:   0x00010000 / 0x0060D340
 *   Entry point:    0x00159E95
 *   .text:          0x00011000, 3.2 MB (game, RenderWare 3.6, CRT, XAPI)
 *   Kernel imports: 151
 *
 * Environment:
 *   DW_GAME_DIR      game data folder (default "game"): default.xbe + *.pck
 *   DW_SAVE_DIR      save folder (default: the runtime's choice)
 *   RECOMP_PB_EXEC   execute the NV2A pushbuffer (needed to see frames)
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#if defined(__PROSPERO__)
/* The console: ps5/ps5_host.c. backtrace is the platform layer's. */
int backtrace(void **frames, int n);
void backtrace_symbols_fd(void *const *frames, int n, int fd);
extern void ps5_host_init(void);
extern int ps5_host_renderer(void);
extern void ps5_crumb(const char *what);
extern void ps5_crumb_fault(int sig, void *si, void *ctx);
#define DW_STEP(s) ps5_crumb(s)
#define DW_RIP(uc) ((uc)->uc_mcontext.mc_rip)
#else
#include <execinfo.h>
#include <sys/syscall.h>
#define DW_RIP(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#define DW_STEP(s) ((void)0)
#endif

#include "xbox_memory_layout.h"
#include "kernel.h"

#define DW_ENTRY_POINT 0x00159E95u

/* XDK D3D's global device pointer (D3D__pDevice), in the D3D section. The
 * device keeps at +0x30 the address of the semaphore its fence code reads the
 * GPU's progress from; see the semaphore watcher below. */
#define DW_D3D_DEVICE_PTR 0x003548F8u

#ifndef RECOMP_REGS_STRUCT
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
#endif
#ifndef RECOMP_REGS_STRUCT
extern __thread uint32_t g_ebx, g_esi, g_edi;
#endif
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int dwvk_register(void);
extern int recomp_dispatch_init(void);
extern void xbox_OhciInit(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* ── fault reporting ─────────────────────────────────────────── */

static void print_guest_stack(void)
{
    if (!g_xbox_mem_offset || !g_esp)
        return;
    const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
    int shown = 0;
    fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
    for (int i = 0; i < 256 && shown < 24; i++) {
        uint32_t v = sp[i];
        if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
            fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
            shown++;
        }
    }
    if (!shown)
        fprintf(stderr, "    (no return addresses in range)\n");
}

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;

    if (xbox_PosixMmioFault(ucv, fault))
        return; /* emulated a device register access; resume */

    ucontext_t *uc = (ucontext_t *)ucv;
#if defined(__PROSPERO__)
    ps5_crumb_fault(sig, si, ucv);
#endif
    fprintf(stderr, "\n[CRASH] signal %d at RIP=0x%llX, fault addr=%p\n", sig,
            (unsigned long long)DW_RIP(uc), si->si_addr);
    fprintf(stderr, "  Xbox regs: eax=%08X ecx=%08X edx=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=%08X esi=%08X edi=%08X\n", g_ebx, g_esi, g_edi);
    if (g_xbox_mem_offset) {
        uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
        char what[128];
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n", va);
        /* Was the memory behind this access, or behind the object the faulting
         * code was walking, already handed back to the heap? A stale pointer
         * reads as a wild address once the block is reused, and the answer
         * decides whether to look for the missing free or for the bad write. */
        xbox_HeapDescribe(va, what, sizeof what);
        fprintf(stderr, "  heap: fault VA %s\n", what);
        xbox_HeapDescribe(g_esi, what, sizeof what);
        fprintf(stderr, "  heap: esi 0x%08X %s\n", g_esi, what);
        xbox_HeapDescribe(g_ebx, what, sizeof what);
        fprintf(stderr, "  heap: ebx 0x%08X %s\n", g_ebx, what);
    }
    print_guest_stack();
    {
        /* The host side: which recompiled function (sub_<guest VA>) or
         * runtime routine faulted, and its callers. Needs -rdynamic. */
        void *frames[32];
        int n = backtrace(frames, 32);
        fprintf(stderr, "  host backtrace:\n");
        backtrace_symbols_fd(frames, n, 2);
    }
    fflush(stderr);
#if defined(__PROSPERO__)
    {
        extern void ps5_log_drain(void);
        ps5_log_drain();
    }
#endif

    signal(sig, SIG_DFL);
    raise(sig);
}

/* The signal stack is per thread: each thread that runs guest code sets one. */
static void install_altstack(void)
{
    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    if (ss.ss_sp)
        sigaltstack(&ss, NULL);
}

static void install_fault_handler(void)
{
    install_altstack();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

/* ── sampling profiler (DW_PROFILE=1) ──────────────────────────── */

/* Every 5 ms: is the guest main thread running or asleep, and which guest
 * code is at the top of its stack. The guest registers are thread-local, so
 * the main thread publishes the address of its g_esp for this thread to read.
 * Reported every 10 s: the share of samples asleep, and the innermost guest
 * return addresses -- a profile of where the game's time goes, waits
 * included, which the runtime's call-count profiler cannot show. */
static volatile uint32_t *s_main_esp;
static pid_t s_main_tid;

#define PROF_SLOTS 4096
static struct { uint32_t va[3], run, sleep; } s_prof[PROF_SLOTS];

/* Guest code, not the D3D section's data that sits in the same range. */
static int is_code(uint32_t v)
{
    return v > g_xbox_code_lo && v < g_xbox_code_hi
        && !(v >= 0x00354000u && v < 0x00357020u);
}

static void prof_note(const uint32_t va[3], int running)
{
    unsigned i = ((va[0] * 2654435761u) ^ (va[1] * 40503u) ^ va[2]) & (PROF_SLOTS - 1), k;
    for (k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (!s_prof[i].va[0] || !memcmp(s_prof[i].va, va, 12)) {
            memcpy(s_prof[i].va, va, 12);
            if (running) s_prof[i].run++; else s_prof[i].sleep++;
            return;
        }
    }
}

static int prof_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return (int)(y[3] + y[4]) - (int)(x[3] + x[4]);
}

/* Set by the D3D kick wrappers (dw_patches.c) while the main thread waits
 * for the pushbuffer walk: that time is the GPU executor's, not the game's. */
extern volatile int g_dw_in_kick_wait;

static int prof_cmp1(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return (int)y[1] - (int)x[1];
}

static void *profiler_thread(void *arg)
{
    unsigned ticks = 0, run = 0, sleep = 0, kick = 0, lost = 0;
    static struct { uint32_t va; unsigned n; } top1[PROF_SLOTS];
    (void)arg;
#if !defined(__PROSPERO__)
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int)s_main_tid);
#endif
    for (;;) {
        struct timespec ts = { 0, 5000000 };
        int running = 1;
        nanosleep(&ts, NULL);
#if !defined(__PROSPERO__)
        {
            char buf[512], *p;
            FILE *f;
            running = 0;
            if ((f = fopen(path, "r"))) {
                if (fgets(buf, sizeof buf, f) && (p = strrchr(buf, ')')))
                    running = p[2] == 'R';
                fclose(f);
            }
        }
#endif
        if (g_dw_in_kick_wait)
            kick++;
        else if (running) run++; else sleep++;
        if (s_main_esp && g_xbox_mem_offset && !g_dw_in_kick_wait) {
            uint32_t esp = *s_main_esp, shown = 0, va[3] = { 0, 0, 0 };
            const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp);
            if (esp > 0x10000 && esp < 0x04000000) {
                for (int i = 0; i < 256 && shown < 3; i++)
                    if (is_code(sp[i]))
                        va[shown++] = sp[i];
                if (shown)
                    prof_note(va, running);
                else
                    lost++;
            }
        }
        if (++ticks % 4000 == 0) {                 /* 20 s */
            static unsigned rows[PROF_SLOTS][5];
            unsigned total = run + sleep + kick, n1 = 0;
            int n = 0, i, k;
            for (i = 0; i < PROF_SLOTS; i++)
                if (s_prof[i].va[0]) {
                    memcpy(rows[n], s_prof[i].va, 12);
                    rows[n][3] = s_prof[i].run;
                    rows[n][4] = s_prof[i].sleep;
                    n++;
                }
            /* Innermost return address alone: which call is the time in. */
            for (i = 0; i < n; i++) {
                for (k = 0; k < (int)n1; k++)
                    if (top1[k].va == rows[i][0])
                        break;
                if (k == (int)n1) {
                    top1[n1].va = rows[i][0];
                    top1[n1].n = 0;
                    n1++;
                }
                top1[k].n += rows[i][3] + rows[i][4];
            }
            qsort(top1, n1, sizeof top1[0], prof_cmp1);
            qsort(rows, n, sizeof rows[0], prof_cmp);
            if (total) {
                fprintf(stderr, "[PROF] main thread, 20 s: %u%% in the GPU kick wait, %u%% "
                        "running%s, %u%% asleep (%u samples, %u with no guest frame). "
                        "Innermost guest return addresses:\n",
                        kick * 100 / total, run * 100 / total,
#if defined(__PROSPERO__)
                        " or blocked",
#else
                        "",
#endif
                        sleep * 100 / total, total, lost);
                for (k = 0; k < (int)n1 && k < 24; k += 4) {
                    fprintf(stderr, "[PROF]  ");
                    for (i = k; i < k + 4 && i < (int)n1; i++)
                        fprintf(stderr, " %08X %4.1f%%", top1[i].va, 100.0 * top1[i].n / total);
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "[PROF]   stacks (innermost first):\n");
                for (i = 0; i < n && i < 12; i++)
                    fprintf(stderr, "[PROF]   %08X < %08X < %08X  %4.1f%%\n",
                            rows[i][0], rows[i][1], rows[i][2],
                            100.0 * (rows[i][3] + rows[i][4]) / total);
                fflush(stderr);
            }
            memset(s_prof, 0, sizeof s_prof);
            run = sleep = kick = lost = 0;
        }
    }
    return NULL;
}

/* ── D3D fence semaphore ───────────────────────────────────────── */

/* XDK D3D waits for the GPU by reading a semaphore that the pushbuffer's
 * BACK_END_WRITE_SEMAPHORE_RELEASE methods update. The executor writes those
 * releases wherever the title project says (nv2a_pb_set_semaphore_target);
 * left unset, D3D never sees progress and its fence wait (0x003487B0, called
 * from RenderWare's present path) spins for good after the first frame.
 * The device is created at runtime, so wait for it, then register the
 * address it holds at +0x30. */
static void *semaphore_watcher(void *arg)
{
    (void)arg;
    for (int i = 0; i < 6000; i++) {          /* up to 60 s */
        uint32_t dev = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset
                                              + DW_D3D_DEVICE_PTR);
        if (dev) {
            uint32_t sem = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset
                                                  + dev + 0x30);
            if (sem) {
                nv2a_pb_set_semaphore_target(sem);
                fprintf(stderr, "[BOOT] D3D device 0x%08X: fence semaphore at "
                                "0x%08X registered with the executor\n", dev, sem);
                return NULL;
            }
        }
        usleep(10000);
    }
    fprintf(stderr, "[BOOT] D3D device never appeared; fence semaphore not registered\n");
    return NULL;
}

/* ── XBE ─────────────────────────────────────────────────────── */

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *out_size = (size_t)size;
    return data;
}

/* The recompiled game. Runs on the thread xbox_HostWindowMain picks; the
 * guest registers are thread-local, so they are set up here. */
static int guest_main(void)
{
    install_altstack();
    g_esp = XBOX_STACK_TOP;
    s_main_esp = &g_esp;
    /* The registers are thread-local, so the watchdog can only reach this
     * thread's set if this thread hands it over. */
    xbox_ThreadRegister("main", &g_esp, &g_eax, &g_ecx, &g_edx, &g_ebx,
                        &g_esi, &g_edi);
    fprintf(stderr, "[BOOT] %ld CPU(s) online\n", (long)sysconf(_SC_NPROCESSORS_ONLN));
    /* Armed here, not in main(): xbox_HostWindowMain runs this function on a
     * thread of its own on the PS5, and the watchdog keeps the address of
     * *its* caller's thread-local registers. Armed from main() it sampled the
     * host thread, which never runs guest code, so every console report came
     * back with esp at the initial stack top and all registers zero. */
    xbox_WatchdogStart();
#if !defined(__PROSPERO__)
    s_main_tid = (pid_t)syscall(SYS_gettid);
#endif
    if (getenv("DW_PROFILE") && atoi(getenv("DW_PROFILE")) != 0) {
        pthread_t t;
        pthread_create(&t, NULL, profiler_thread, NULL);
        pthread_detach(t);
    }
    printf("Entry 0x%08X, esp 0x%08X. Starting.\n", DW_ENTRY_POINT, g_esp);
    DW_STEP("game thread: entering the game");
    xbe_entry_point();
    return 0;
}

extern int xbox_HostWindowMain(int (*guest_main)(void));

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
#if defined(__PROSPERO__)
    ps5_host_init();
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *game_dir = getenv("DW_GAME_DIR") ? getenv("DW_GAME_DIR") : "game";
    const char *save_dir = getenv("DW_SAVE_DIR");
    char xbe_path[4096];
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game_dir);

    printf("=== Darkwatch - static recompilation (POSIX host) ===\n");
    install_fault_handler();

    size_t xbe_size = 0;
    void *xbe = load_file(xbe_path, &xbe_size);
    if (!xbe) {
        fprintf(stderr, "Cannot read %s. Put default.xbe and the .pck files in %s/.\n",
                xbe_path, game_dir);
        return 1;
    }
    printf("XBE: %zu bytes\n", xbe_size);
    DW_STEP("xbe loaded");

    DW_STEP("memory layout: start");
    /* 128 MB of guest address space over the console's 64 MB of RAM, so the
     * heap can run past 0x04000000 once the low 48 MB is used up. The title
     * is still told it has 64 MB, and the allocator only goes above it when
     * the low range is exhausted. Darkwatch's level cache (CacheFile.cpp)
     * churns 1 MB buffers, and its heap reservations are charged in full here
     * though the console only pays for committed pages: by the third level a
     * buffer could not be had, the cache asserted (bufferIndex <
     * m_cacheBufferCount) and the title faulted. The layout reserves the
     * whole guest window when the map exceeds RAM. RECOMP_MAP_MB=64 turns it
     * off; 65..256 picks another size. */
    long map_mb = 64;
    {
        const char *e = getenv("RECOMP_MAP_MB");
        long mb = e && *e ? atol(e) : 128;
        if (mb > 64 && mb <= 256) {
            xbox_SetMapSize((size_t)mb << 20);
            map_mb = mb;
        }
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        /* A larger map that this host cannot place: the console's own
         * 64 MB still runs the game, with less room for the heap. */
        int ok = 0;
        if (map_mb > 64) {
            fprintf(stderr, "[BOOT] a %ld MB guest map failed; retrying with 64 MB\n", map_mb);
            xbox_MemoryLayoutShutdown();
            xbox_SetMapSize(0);
            map_mb = 64;
            ok = xbox_MemoryLayoutInit(xbe, xbe_size);
        }
        if (!ok) {
            fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
            return 1;
        }
    }
    fprintf(stderr, "[BOOT] guest map %ld MB\n", map_mb);
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Guest memory at offset 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    DW_STEP("memory layout done");

    /* The emulated MCPX APU (xemu's model). DirectSound and XACT are linked
     * into the XBE and program the APU directly; with RECOMP_AC97_READY the
     * memory layout traps the APU registers and xbox_PosixMmioFault routes
     * them here. Without it DirectSound finds no codec. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    DW_STEP("apu done");
    xbox_kernel_init();
    DW_STEP("kernel init done");
    xbox_path_init(game_dir, save_dir);
    xbox_kernel_bridge_init();
    DW_STEP("kernel bridge done");
    /* The USB host controllers XAPI drives for the gamepad (RECOMP_USB=1):
     * trapped registers, answered by the toolkit's OHCI model. */
    xbox_OhciInit();

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    DW_STEP("dispatch + watchdog done");

    /* DW_RENDERER=vulkan: rasterise on the GPU (src/gpu/nv2a_vulkan.c)
     * instead of the executor's CPU rasteriser. */
    if (getenv("RECOMP_PB_EXEC") && getenv("DW_RENDERER")
        && !strcmp(getenv("DW_RENDERER"), "vulkan")) {
#if defined(__PROSPERO__)
        DW_STEP("renderer: starting Vulkan");
        ps5_host_renderer();
        DW_STEP("renderer: done");
#else
        dwvk_register();
#endif
    }

    if (getenv("RECOMP_PB_EXEC")) {
        pthread_t t;
        pthread_create(&t, NULL, semaphore_watcher, NULL);
        pthread_detach(t);
    }

    /* With a display (and RECOMP_FB_WINDOW, which run_darkwatch.sh sets),
     * the main thread owns an SDL window and the game runs on a pthread;
     * headless, the game runs right here. */
    DW_STEP("starting the game thread");
    xbox_HostWindowMain(guest_main);

    printf("Game returned.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}

/* The D3D device's pushbuffer write pointer (device+0), for the walker's
 * desync reports. */
uint32_t nv2a_pb_title_write_ptr(void)
{
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + DW_D3D_DEVICE_PTR);
    if (!dev) return 0;
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + dev);
}
