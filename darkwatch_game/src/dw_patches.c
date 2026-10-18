/*
 * Darkwatch - patches to the recompiled game, by link-time wrapping
 * (-Wl,--wrap=sub_XXXXXXXX: calls from other units reach __wrap_, which can
 * call the lifted function as __real_).
 *
 * sub_003646A1  WSAWaitForMultipleEvents (XNet; a tail jump to XAPI's
 *               WaitForMultipleObjectsEx)
 *   After the intro the game services its sockets in a loop that repeats
 *   while WSAWaitForMultipleEvents(8, events, FALSE, 0, FALSE) reports an
 *   event (0x001AB6A0, called from 0x001A3B5F). With no network behind XNet
 *   here, an overlapped receive completes at once with WSAEWOULDBLOCK and
 *   signals its event, so the poll never comes back empty and the loop never
 *   ends: no frame is drawn again (both on PC and on the PS5). From that one
 *   call site, a zero-timeout poll reports WAIT_TIMEOUT, which is what an
 *   unplugged console's network stack would say: nothing arrived.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include "recomp_funcs.h"

void __real_sub_003646A1(void);
void __wrap_sub_003646A1(void);

void __wrap_sub_003646A1(void)
{
    uint32_t ret_va  = MEM32(g_esp);        /* the caller's return address */
    uint32_t timeout = MEM32(g_esp + 16);   /* dwTimeout, the fourth argument */

    if (ret_va == 0x001AB6F9u && timeout == 0) {
        static int noted;
        if (!noted++)
            fprintf(stderr, "[DW] socket poll after the intro: reporting no network "
                            "traffic (WAIT_TIMEOUT)\n");
        g_eax = 0x102;              /* WAIT_TIMEOUT */
        g_esp += 4 + 5 * 4;         /* ret 0x14: the return address and 5 args */
        return;
    }
    __real_sub_003646A1();
}

/*
 * sub_001AB6A0  the socket pump's "anything to service?" test (thiscall,
 *               one argument: the timeout)
 *   The loop at 0x001A3AE2..0x001A3B66 runs while this says yes. One of its
 *   eight request slots reports state 2 (ready) every time, its receive
 *   completes with WSAEWOULDBLOCK, and the slot is ready again, so the loop
 *   never ends. A pump that has said yes 256 times in a row is told no: the
 *   frame goes on, and the next frame pumps again.
 */
void __real_sub_001AB6A0(void);
void __wrap_sub_001AB6A0(void);

void __wrap_sub_001AB6A0(void)
{
    static uint32_t run;
    static unsigned long long broken;

    if (run >= 256) {
        run = 0;
        if (broken++ < 3)
            fprintf(stderr, "[DW] socket pump: still busy after 256 rounds; ending "
                            "this frame's pump\n");
        g_eax &= ~0xFFu;            /* al = 0: nothing to service */
        g_esp += 4 + 4;             /* ret 4 */
        return;
    }
    __real_sub_001AB6A0();
    if (g_eax & 0xFFu)
        run++;
    else
        run = 0;
}

/*
 * The gamepad, high level.
 *
 * XAPI's input stack (XPP, linked into the XBE) finds controllers by driving
 * the MCPX USB host controllers itself. The runtime's OHCI model gets the
 * port reset done, but XPP then never queues the first control transfer, so
 * enumeration never finishes and the title waits on it (RECOMP_USB=1). So
 * here the XPP entry points the title calls are answered directly for the
 * gamepad type (XDEVICE_TYPE_GAMEPAD at 0x0040D37C), from the host's
 * controller (SDL on a PC, the DualSense on a PS5) or the keyboard:
 *
 *   0x0040E5B2  XGetDevices(type)                     port 0 present
 *   0x0040EA44  XInputOpen(type, port, slot, params)  a handle for port 0
 *   0x0040EAA6  XInputGetState(handle, state)         the host pad
 *   0x0040EB19  XInputSetState(handle, feedback)      rumble
 *   0x0040EA9A  XInputClose(handle)
 *
 * Other device types (memory units, the headset) go to XPP as before, which
 * reports none.
 */
#include "xinput_xbox.h"

#define DW_GAMEPAD_TYPE 0x0040D37Cu
#define DW_PAD_HANDLE   0x0DEAD0A0u      /* never dereferenced by the title */

static void dw_pad_init(void)
{
    static int done;
    if (!done) {
        done = 1;
        xbox_InputInit();
        fprintf(stderr, "[DW] gamepad: XPP's XInput answered from the host controller\n");
    }
}

void __real_sub_0040E5B2(void);
void __wrap_sub_0040E5B2(void);
void __wrap_sub_0040E5B2(void)
{
    uint32_t type = MEM32(g_esp + 4);
    if (type == DW_GAMEPAD_TYPE) {
        dw_pad_init();
        MEM32(type + 0) = 1;            /* CurrentConnected: port 0 */
        MEM32(type + 4) = 0;
        MEM32(type + 8) = 1;
        g_eax = 1;
        g_esp += 4 + 4;                 /* ret 4 */
        return;
    }
    __real_sub_0040E5B2();
}

void __real_sub_0040EA44(void);
void __wrap_sub_0040EA44(void);
void __wrap_sub_0040EA44(void)
{
    uint32_t type = MEM32(g_esp + 4), port = MEM32(g_esp + 8);
    if (type == DW_GAMEPAD_TYPE) {
        dw_pad_init();
        g_eax = port == 0 ? DW_PAD_HANDLE : 0;
        g_esp += 4 + 16;                /* ret 0x10 */
        return;
    }
    __real_sub_0040EA44();
}

/* DW_PAD_PRESS="START:95,A:100,DOWN:104.5,RSR:150+4": a button held for 0.3 s
 * (or +seconds) at each time (seconds since start) -- for headless test runs. */
#include <stdlib.h>
#include <time.h>
static void dw_pad_script(XBOX_INPUT_STATE *s)
{
    static const char *spec = (const char *)-1;
    static double t0;
    struct timespec ts;
    double now;
    const char *p;

    if (spec == (const char *)-1)
        spec = getenv("DW_PAD_PRESS");
    if (!spec)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (!t0) {
        extern double dw_start_time(void);
        t0 = dw_start_time();
    }
    for (p = spec; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char name[16];
        double at, len = 0.3;
        int n = 0;
        if (sscanf(p, "%15[A-Z]:%lf%n", name, &at, &n) < 2)
            continue;
        if (p[n] == '+')                 /* NAME:at+seconds holds it longer */
            len = atof(p + n + 1);
        if (now - t0 < at || now - t0 > at + len)
            continue;
        /* Sticks: RSL/RSR turn, LSU/LSD walk (two-thirds over). */
        if (!strcmp(name, "RSL")) s->Gamepad.sThumbRX = -22000;
        else if (!strcmp(name, "RSR")) s->Gamepad.sThumbRX = 22000;
        else if (!strcmp(name, "LSU")) s->Gamepad.sThumbLY = 22000;
        else if (!strcmp(name, "LSD")) s->Gamepad.sThumbLY = -22000;
        if (!strcmp(name, "START")) s->Gamepad.wButtons |= XBOX_GAMEPAD_START;
        else if (!strcmp(name, "BACK")) s->Gamepad.wButtons |= XBOX_GAMEPAD_BACK;
        else if (!strcmp(name, "UP")) s->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
        else if (!strcmp(name, "DOWN")) s->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        else if (!strcmp(name, "LEFT")) s->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
        else if (!strcmp(name, "RIGHT")) s->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
        else if (!strcmp(name, "A")) s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        else if (!strcmp(name, "B")) s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
        else if (!strcmp(name, "X")) s->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        else if (!strcmp(name, "Y")) s->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
        else if (!strcmp(name, "RT")) s->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
    }
}

void __real_sub_0040EAA6(void);
void __wrap_sub_0040EAA6(void);
void __wrap_sub_0040EAA6(void)
{
    uint32_t h = MEM32(g_esp + 4), st = MEM32(g_esp + 8);
    if (h == DW_PAD_HANDLE) {
        XBOX_INPUT_STATE s;
        DWORD r;
        int i;
        memset(&s, 0, sizeof s);
        r = xbox_InputGetState(0, &s);
        dw_pad_script(&s);
        if (st) {
            /* XINPUT_STATE as the title lays it out: packet, buttons,
             * 8 analog buttons, 4 stick axes (22 bytes). */
            MEM32(st + 0) = s.dwPacketNumber;
            MEM16(st + 4) = s.Gamepad.wButtons;
            for (i = 0; i < 8; i++)
                MEM8(st + 6 + i) = s.Gamepad.bAnalogButtons[i];
            MEM16(st + 14) = (uint16_t)s.Gamepad.sThumbLX;
            MEM16(st + 16) = (uint16_t)s.Gamepad.sThumbLY;
            MEM16(st + 18) = (uint16_t)s.Gamepad.sThumbRX;
            MEM16(st + 20) = (uint16_t)s.Gamepad.sThumbRY;
        }
        /* A pad that is not there reads as one at rest: the title opened it. */
        (void)r;
        g_eax = 0;
        g_esp += 4 + 8;                 /* ret 8 */
        return;
    }
    __real_sub_0040EAA6();
}

void __real_sub_0040EB19(void);
void __wrap_sub_0040EB19(void);
void __wrap_sub_0040EB19(void)
{
    uint32_t h = MEM32(g_esp + 4), fb = MEM32(g_esp + 8);
    if (h == DW_PAD_HANDLE) {
        if (fb) {
            XBOX_VIBRATION v;
            v.wLeftMotorSpeed = MEM16(fb + 0x42);
            v.wRightMotorSpeed = MEM16(fb + 0x44);
            xbox_InputSetState(0, &v);
            MEM32(fb + 0) = 0;          /* header dwStatus: done */
        }
        g_eax = 997;                    /* ERROR_IO_PENDING, already complete */
        g_esp += 4 + 8;                 /* ret 8 */
        return;
    }
    __real_sub_0040EB19();
}

void __real_sub_0040EA9A(void);
void __wrap_sub_0040EA9A(void);
void __wrap_sub_0040EA9A(void)
{
    if (MEM32(g_esp + 4) == DW_PAD_HANDLE) {
        g_esp += 4 + 4;                 /* ret 4 */
        return;
    }
    __real_sub_0040EA9A();
}

/* The time the process started, for DW_PAD_PRESS. */
static double s_dw_t0;
__attribute__((constructor)) static void dw_t0_init(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_dw_t0 = ts.tv_sec + ts.tv_nsec / 1e9;
}
double dw_start_time(void) { return s_dw_t0; }

/*
 * sub_003A9841  DirectSoundDoWork
 *   Every call checks a word in the EP DSP's memory (0xFE85A018) for
 *   0xCCCCCC, the value the running EP program keeps there. The DSPs are not
 *   emulated (their memory is plain RAM), so the check failed every frame and
 *   DirectSound restarted the EP each time: a 10 ms stall, all AC'97 channels
 *   reset, and the voices it was playing cut off -- the frame rate halved in
 *   menus, the audio crackled, and starting a level waited forever on sound
 *   that never finished. The word is kept at its "EP running" value.
 */
void __real_sub_003A9841(void);
void __wrap_sub_003A9841(void);

void __wrap_sub_003A9841(void)
{
    if (MEM32(0xFE85A018u) != 0x00CCCCCCu)
        MEM32(0xFE85A018u) = 0x00CCCCCCu;
    __real_sub_003A9841();
}

/* The guest stack's return addresses (code VAs), innermost first: where a
 * runtime report (a null indirect call, ...) came from. */
void dw_guest_stack_dump(const char *tag)
{
    uint32_t a, k = 0;
    fprintf(stderr, "%s", tag);
    for (a = 0; a < 64 * 4 && k < 12; a += 4) {
        uint32_t v = MEM32(g_esp + a);
        if (v >= 0x11000u && v < 0x450000u) {
            fprintf(stderr, " %08X", v);
            k++;
        }
    }
    fprintf(stderr, "\n");
}

/* Darkwatch's D3D keeps its own copy of the GPU's pushbuffer read pointer:
 * sub_003487B0 reads the device at [0x003548F8], takes PUT from +0x2C and
 * reads GET through the pointer at +0x30 -- a word in guest memory that
 * nothing here updates, which is why the title eventually decides the ring is
 * full and waits for ever.
 *
 * Mirroring the title's own PUT into that word (xbox_Nv2aMirrorFence) does
 * unblock it, and on the PC it ran a whole level: 190k draws, no stall. On
 * the console it wrecked the picture -- artifacts, glitched colour, a hang
 * and then a crash -- because it tells the title the GPU has consumed
 * everything, which is permission to overwrite commands the executor has not
 * read yet. The console runs far enough ahead to take that permission.
 *
 * So it is off. The honest value is the executor's real published position,
 * but a first attempt at that collapsed the PC to 3k draws (the two sides of
 * the comparison are in different address spaces and the conversion was
 * wrong), so it needs the device's actual field values from a console
 * capture before being tried again. See NEXT_SESSION.md.
 */
#if 0
__attribute__((constructor)) static void dw_fence_mirror(void)
{
    extern int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                                    uint32_t put_off, uint32_t get_ptr_off);
    xbox_Nv2aMirrorFence(0x003548F8u, 0x2Cu, 0x30u);
}
#endif

/*
 * sub_003485D0  D3D KickOff (writes the channel's DMA_PUT)
 * sub_00348680  D3D KickOff-and-wait-for-idle
 *
 * On the Xbox the GPU runs alongside the CPU and D3D trusts it to keep up:
 * it decides how much of the 2 MB pushbuffer ring it may reuse from fence
 * records in a 64-entry table, and its "is the GPU busy?" test
 * (sub_00344F40) reads PFIFO CACHE1 PUT/GET, which nothing here models.
 * Our GPU is a walk on the poll thread that executes the commands, draws
 * included, and can run well behind the title. When it did, D3D took
 * recycled fence records as passed and wrote the next lap of the ring over
 * commands the walk had not yet read -- every pushbuffer desync in the
 * console logs ("bad target ... resync") was the walk reading index data
 * that had landed on top of a half-read frame, and each one lost fences and
 * hung the title in its fence wait.
 *
 * So the kick waits for the walk to reach the PUT it just wrote, which is
 * the guarantee D3D was relying on. Bounded, so a walk that is held (a
 * software-method interrupt the title's ISR has not taken yet) costs a late
 * frame rather than a hang.
 */
extern int nv2a_pb_walk_done(uint32_t put_phys);
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

volatile int g_dw_in_kick_wait;

static void dw_kick_wait_body(const char *who);
static void dw_kick_wait(const char *who)
{
    g_dw_in_kick_wait = 1;
    dw_kick_wait_body(who);
    g_dw_in_kick_wait = 0;
}

static void dw_kick_wait_body(const char *who)
{
    static unsigned timeouts;
    static uint64_t waits, waited_us;
    volatile uint32_t *put_reg = xbox_Nv2aRegPtr(0x800040u);
    struct timespec t0, t;
    uint32_t put;
    unsigned spins = 0;

    if (!put_reg)
        return;
    put = *put_reg;
    if (nv2a_pb_walk_done(put))
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint64_t us;
        if (nv2a_pb_walk_done(put))
            break;
        /* Spin briefly on the CPU, then sleep in short steps: a
         * sched_yield loop is a system call per turn on the PS5 and takes
         * the core the walk wants. */
        if (++spins < 4000) {
            __builtin_ia32_pause();
            continue;
        }
        usleep(50);
        clock_gettime(CLOCK_MONOTONIC, &t);
        us = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000u
           + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000);
        if (us > 250000u) {
            if (timeouts++ < 8 || (timeouts % 256) == 0)
                fprintf(stderr, "[DW] %s: the pushbuffer walk did not reach PUT %08X "
                                "within 250 ms; letting the title go on (#%u)\n",
                        who, put, timeouts);
            break;
        }
        if (*put_reg != put)          /* another kick moved PUT on */
            put = *put_reg;
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    waits++;
    waited_us += (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000u
               + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000);
    if ((waits & (waits - 1)) == 0 && waits >= 64)
        fprintf(stderr, "[DW] kick waits: %llu, %.1f ms in all (%.0f us each)\n",
                (unsigned long long)waits, waited_us / 1000.0,
                (double)waited_us / (double)waits);
}

/* DW_KICK_LAG=<n> (0..4, default 0): a KickOff waits only for the walk to
 * finish the kick n before it, not its own -- the title builds the next
 * stretch of commands while the executor draws the last, as the console's
 * CPU and GPU overlap. The executor's GET is published as it goes and the
 * D3D fence semaphore is written as releases execute, which is what D3D
 * itself checks before reusing the ring or a resource. KickOffAndWait
 * (D3D's "wait for idle") still waits for everything. */
extern volatile uint32_t g_nv2a_kick_seq, g_nv2a_walked_seq;

static int kick_lag(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DW_KICK_LAG");
        v = e ? atoi(e) : 0;
        if (v < 0) v = 0;
        if (v > 4) v = 4;
        if (v)
            fprintf(stderr, "[DW] KickOff waits for the kick %d before it (DW_KICK_LAG)\n", v);
    }
    return v;
}

static void dw_kick_wait_seq(const char *who, uint32_t want)
{
    static unsigned timeouts;
    static uint64_t waits, waited_us;
    struct timespec t0, t;
    unsigned spins = 0;

    if ((int32_t)(__atomic_load_n(&g_nv2a_walked_seq, __ATOMIC_ACQUIRE) - want) >= 0)
        return;
    g_dw_in_kick_wait = 1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint64_t us;
        if ((int32_t)(__atomic_load_n(&g_nv2a_walked_seq, __ATOMIC_ACQUIRE) - want) >= 0)
            break;
        if (++spins < 4000) {
            __builtin_ia32_pause();
            continue;
        }
        usleep(50);
        clock_gettime(CLOCK_MONOTONIC, &t);
        us = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000u
           + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000);
        if (us > 250000u) {
            if (timeouts++ < 8 || (timeouts % 256) == 0)
                fprintf(stderr, "[DW] %s: the pushbuffer walk did not finish kick %u "
                                "within 250 ms; letting the title go on (#%u)\n",
                        who, want, timeouts);
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    g_dw_in_kick_wait = 0;
    waits++;
    waited_us += (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000u
               + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000);
    if ((waits & (waits - 1)) == 0 && waits >= 64)
        fprintf(stderr, "[DW] lagged kick waits: %llu, %.1f ms in all (%.0f us each)\n",
                (unsigned long long)waits, waited_us / 1000.0,
                (double)waited_us / (double)waits);
}

void __real_sub_003485D0(void);
void __wrap_sub_003485D0(void);
void __wrap_sub_003485D0(void)
{
    uint32_t seq;
    __real_sub_003485D0();
    seq = __atomic_add_fetch(&g_nv2a_kick_seq, 1, __ATOMIC_RELEASE);
    if (kick_lag())
        dw_kick_wait_seq("KickOff", seq - (uint32_t)kick_lag());
    else
        dw_kick_wait("KickOff");
}

void __real_sub_00348680(void);
void __wrap_sub_00348680(void);
void __wrap_sub_00348680(void)
{
    __real_sub_00348680();
    __atomic_add_fetch(&g_nv2a_kick_seq, 1, __ATOMIC_RELEASE);
    dw_kick_wait("KickOffAndWait");
}

/* For the not-code ICALL report: a virtual call through a bad vtable is the
 * usual shape, so show the object ecx points at and the table it claims --
 * what kind of object it was, and whether it had been freed. */
void dw_icall_object_dump(void)
{
    extern void xbox_HeapDescribe(uint32_t xbox_va, char *out, size_t out_size);
    uint32_t obj = g_ecx, vt, k;
    char buf[256];

    if (obj < 0x10000u || obj >= 0x08000000u)
        return;
    vt = MEM32(obj);
    fprintf(stderr, "  [ICALL] ecx=%08X:", obj);
    for (k = 0; k < 12; k++)
        fprintf(stderr, " %08X", MEM32(obj + 4 * k));
    fprintf(stderr, "\n");
    if (vt >= 0x10000u && vt < 0x08000000u) {
        fprintf(stderr, "  [ICALL] vtable %08X:", vt);
        for (k = 0; k < 8; k++)
            fprintf(stderr, " %08X", MEM32(vt + 4 * k));
        fprintf(stderr, "\n");
    }
    buf[0] = 0;
    xbox_HeapDescribe(obj, buf, sizeof buf);
    fprintf(stderr, "  [ICALL] heap: object %s\n", buf);
}

/*
 * The hang at the door before the train cinematic: the main thread loops in
 * sub_001D2B78 -- while (obj->busy [+0x1A]) { Sleep(0); obj->vtbl[5]() } --
 * and the pump (0x003B0381, DirectSound) only moves on once the voice's
 * notifier #3 reads DONE (sub_003ACD83). Our APU writes notifiers 0/1
 * (segment done) itself; #3 is not one it writes. Once the wait has gone on
 * for 3 s, say what the object is and what the APU thinks of its voice.
 *
 * Found through the Sleep: sub_0015B5C9 is a framed function whose return
 * ([ebp+4]) is 0x001D2B86 inside the loop, and it saves the caller's esi --
 * the object -- at [ebp-12].
 */
void xbox_title_delay_hook(void)
{
    static uint64_t first_ms;
    static unsigned reports;
    struct timespec ts;
    uint64_t now;
    uint32_t f = g_ebp, obj, nb, k;

    if (f < 0x1000u || f >= 0x08000000u || MEM32(f + 4) != 0x001D2B86u) {
        first_ms = 0;
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (!first_ms) { first_ms = now; return; }
    if (now - first_ms < 3000u || reports >= 3)
        return;
    if (reports && now - first_ms < 3000u * (reports + 1u))
        return;
    reports++;
    obj = MEM32(f - 12);
    fprintf(stderr, "[DW] waiting %llu ms at the door loop (sub_001D2B78) on object %08X, vtable %08X, busy byte %02X\n",
            (unsigned long long)(now - first_ms), obj, obj ? MEM32(obj) : 0, obj ? MEM8(obj + 0x1A) : 0);
    if (obj < 0x10000u || obj >= 0x08000000u)
        return;
    for (k = 0; k < 0xA0; k += 32)
        fprintf(stderr, "[DW]   +%02X: %08X %08X %08X %08X %08X %08X %08X %08X\n", k,
                MEM32(obj + k), MEM32(obj + k + 4), MEM32(obj + k + 8), MEM32(obj + k + 12),
                MEM32(obj + k + 16), MEM32(obj + k + 20), MEM32(obj + k + 24), MEM32(obj + k + 28));
    nb = MEM32(obj + 0x68);
    if (nb >= 0x10000u) {
        extern void mcpx_apu_voice_describe(uint32_t notifier_va, char *out, size_t n);
        char buf[256];
        for (k = 0; k < 4; k++)
            fprintf(stderr, "[DW]   notifier %u at %08X: %08X %08X %08X %08X (status byte %02X)\n", k, nb + 16 * k,
                    MEM32(nb + 16 * k), MEM32(nb + 16 * k + 4), MEM32(nb + 16 * k + 8), MEM32(nb + 16 * k + 12),
                    MEM8(nb + 16 * k + 15));
        mcpx_apu_voice_describe(nb, buf, sizeof buf);
        fprintf(stderr, "[DW]   APU: %s\n", buf);
    }
    fflush(stderr);
}
