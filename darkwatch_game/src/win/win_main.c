/*
 * Darkwatch (USA, CC-022) - static recompilation, Windows host.
 *
 * The desktop counterpart of src/main_posix.c and ps5/ps5_host.c:
 *
 *   - one window, owned by the process's main thread, which pumps its
 *     messages for the life of the process;
 *   - the launcher (launcher.c) draws into that window first, and its
 *     settings (dw_settings.c) become the runtime's switches;
 *   - then the same boot as the other hosts (XBE, guest memory, APU,
 *     kernel, paths, kernel bridge, USB, dispatch), with the Vulkan back end
 *     presenting into the window through VK_KHR_win32_surface -- the PS5's
 *     present path: the picture rendered at N x the console's resolution and
 *     interpolated up to the display's refresh (or the frame cap);
 *   - the recompiled game on a thread of its own with a 64 MB stack.
 *
 * Keys: F11 or Alt+Enter toggles borderless fullscreen, F9 releases the
 * mouse (mkb_input.c), Alt+F4 quits.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "xbox_memory_layout.h"
#include "kernel.h"
#include "keyboard.h"
#include "mkb_input.h"
#include "nv2a_backend.h"

#include "dw_settings.h"
#include "launcher.h"
#include "win_host.h"

#define DW_ENTRY_POINT    0x00159E95u
#define DW_D3D_DEVICE_PTR 0x003548F8u

extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int recomp_dispatch_init(void);
extern void xbox_OhciInit(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern void xa2_set_volume(float v);

typedef VkSurfaceKHR (*dwvk_surface_fn)(VkInstance, PFN_vkGetInstanceProcAddr,
                                        VkPhysicalDevice, VkExtent2D *);
extern void dwvk_set_present(const char *const *exts, uint32_t n, dwvk_surface_fn fn);
extern int dwvk_register(void);
extern int dwvk_present(uint32_t fb_va, uint32_t pitch, uint32_t height);
extern int dwvk_presenting(void);
extern void dwvk_window_resized(void);
extern void dwvk_set_display_options(int vsync, int fps_cap, int aspect);
extern unsigned dwvk_display_frames(void);

HWND g_dw_hwnd;
static volatile LONG s_game_running;
static int s_fullscreen;
static WINDOWPLACEMENT s_wp = { sizeof(WINDOWPLACEMENT) };
static volatile LONG s_flips;
static volatile LONG s_draws;
static volatile int s_overlay;          /* the F1 settings over the game */
static volatile uint32_t s_last_fb_va, s_last_fb_pitch;

/* ── log ─────────────────────────────────────────────────────────── */

static char s_log_path[DW_PATH_MAX + 32];

static DWORD WINAPI log_flusher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(250);
        fflush(stderr);
        fflush(stdout);
    }
    return 0;
}

static void log_open(void)
{
    char prev[DW_PATH_MAX + 32];

    if (g_dw.log_console || getenv("DW_CONSOLE")) {
        if (AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }
    /* darkwatch-log.txt beside the exe; the last run's moves aside, so a
     * crash report survives one restart. */
    snprintf(s_log_path, sizeof s_log_path, "%s\\darkwatch-log.txt", dw_exe_dir());
    snprintf(prev, sizeof prev, "%s\\darkwatch-log-prev.txt", dw_exe_dir());
    MoveFileExA(s_log_path, prev, MOVEFILE_REPLACE_EXISTING);
    if (!freopen(s_log_path, "w", stderr))
        return;
    _dup2(_fileno(stderr), _fileno(stdout));
    /* Buffered, flushed four times a second: the runtime logs a lot, and a
     * write per line costs the threads that log it. A crash flushes. */
    setvbuf(stderr, NULL, _IOFBF, 1 << 18);
    setvbuf(stdout, NULL, _IONBF, 0);
    CloseHandle(CreateThread(NULL, 0, log_flusher, NULL, 0, NULL));
}

/* ── crash reports ───────────────────────────────────────────────── */

static void print_guest_stack(void)
{
    const uint32_t *sp;
    int shown = 0, i;
    if (!g_xbox_mem_offset || !g_esp)
        return;
    sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
    fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
    for (i = 0; i < 256 && shown < 24; i++) {
        uint32_t v = sp[i];
        if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
            fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
            shown++;
        }
    }
    if (!shown) {
        uint32_t k;
        fprintf(stderr, "    (no return addresses in range)\n  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
}

/* A host address as module+offset (the exe's offsets resolve against
 * darkwatch.map from the build), and a symbol name when dbghelp has one. */
static void print_host_symbol(void *rip)
{
    char buf[sizeof(SYMBOL_INFO) + 256], mod[MAX_PATH] = "?";
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    HMODULE m = NULL;
    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m) && m) {
        char *b;
        GetModuleFileNameA(m, mod, sizeof mod);
        b = strrchr(mod, '\\');
        fprintf(stderr, "    %s+0x%llX", b ? b + 1 : mod,
                (unsigned long long)((uintptr_t)rip - (uintptr_t)m));
    } else {
        fprintf(stderr, "    0x%llX", (unsigned long long)(uintptr_t)rip);
    }
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  %s+0x%llX", sym->Name, (unsigned long long)disp);
    fprintf(stderr, "\n");
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        /* Trapped device registers (APU, AC97, USB): the access is emulated
         * and the instruction stepped over. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;

    fprintf(stderr, "\n[CRASH] exception 0x%08lX at RIP=0x%llX", (unsigned long)er->ExceptionCode,
            (unsigned long long)ep->ContextRecord->Rip);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, ", %s of 0x%llX", er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)fault);
    fprintf(stderr, "\n");
    print_host_symbol((void *)ep->ContextRecord->Rip);
    fprintf(stderr, "  Xbox regs: eax=%08X ecx=%08X edx=%08X esp=%08X\n", g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=%08X esi=%08X edi=%08X\n", g_ebx, g_esi, g_edi);
    if (g_xbox_mem_offset && fault) {
        uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
        char what[128];
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n", va);
        xbox_HeapDescribe(va, what, sizeof what);
        fprintf(stderr, "  heap: fault VA %s\n", what);
        xbox_HeapDescribe(g_esi, what, sizeof what);
        fprintf(stderr, "  heap: esi 0x%08X %s\n", g_esi, what);
    }
    print_guest_stack();
    {
        /* The host frames: the recompiled function (sub_<guest VA>) and
         * its callers. */
        void *frames[24];
        USHORT n = CaptureStackBackTrace(0, 24, frames, NULL), i;
        fprintf(stderr, "  host backtrace:\n");
        for (i = 0; i < n; i++)
            print_host_symbol(frames[i]);
    }
    fflush(stderr);
    {
        char msg[1024];
        snprintf(msg, sizeof msg,
                 "Darkwatch has crashed (exception 0x%08lX).\n\n"
                 "The details are in:\n%s\n\nPlease keep that file if you report the problem.",
                 (unsigned long)er->ExceptionCode, s_log_path[0] ? s_log_path : "the console");
        if (g_dw_hwnd)
            ShowWindow(g_dw_hwnd, SW_MINIMIZE);
        MessageBoxA(NULL, msg, "Darkwatch", MB_ICONERROR | MB_OK | MB_TOPMOST);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ── the window ──────────────────────────────────────────────────── */

static void monitor_rect(HWND h, RECT *r)
{
    MONITORINFO mi = { sizeof mi };
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoA(m, &mi))
        *r = mi.rcMonitor;
    else
        SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

int dw_display_height(void)
{
    RECT r;
    monitor_rect(g_dw_hwnd, &r);
    return r.bottom - r.top;
}

int dw_display_refresh(void)
{
    DEVMODEA dm;
    MONITORINFOEXA mi;
    HMONITOR m = MonitorFromWindow(g_dw_hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoA(m, (MONITORINFO *)&mi)
        && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
        && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

void dw_set_fullscreen(int on)
{
    HWND h = g_dw_hwnd;
    LONG style = GetWindowLongA(h, GWL_STYLE);

    if (on == s_fullscreen)
        return;
    if (on) {
        RECT r;
        GetWindowPlacement(h, &s_wp);
        monitor_rect(h, &r);
        SetWindowLongA(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &s_wp);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    s_fullscreen = on;
    g_dw.fullscreen = on;
    dwvk_window_resized();
}

int dw_is_fullscreen(void)
{
    return s_fullscreen;
}

static void cursor_update(void)
{
    static int clipped;
    int want = s_game_running && mkb_capture_wanted() && GetForegroundWindow() == g_dw_hwnd;
    if (want) {
        RECT r;
        POINT a, b;
        GetClientRect(g_dw_hwnd, &r);
        a.x = r.left; a.y = r.top; b.x = r.right; b.y = r.bottom;
        ClientToScreen(g_dw_hwnd, &a);
        ClientToScreen(g_dw_hwnd, &b);
        SetRect(&r, a.x, a.y, b.x, b.y);
        ClipCursor(&r);
        clipped = 1;
    } else if (clipped) {
        ClipCursor(NULL);
        clipped = 0;
    }
}

/* The guest framebuffer drawn with GDI: only when there is no Vulkan display
 * (no driver), from the frame the last flip copied out. */
static uint32_t *s_gdi_frame[2];
static volatile LONG s_gdi_idx = -1;

static void gdi_paint(HDC dc)
{
    RECT r;
    BITMAPINFO bi;
    int W, H, dw, dh;
    LONG idx = s_gdi_idx;

    GetClientRect(g_dw_hwnd, &r);
    W = r.right; H = r.bottom;
    dh = H; dw = H * 4 / 3;
    if (dw > W) { dw = W; dh = W * 3 / 4; }
    PatBlt(dc, 0, 0, W, H, BLACKNESS);
    if (idx < 0 || !s_gdi_frame[idx])
        return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 640;
    bi.bmiHeader.biHeight = -480;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, (W - dw) / 2, (H - dh) / 2, dw, dh, 0, 0, 640, 480,
                  s_gdi_frame[idx], &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* ── the F1 overlay ──────────────────────────────────────────────── */

extern void dwvk_overlay_set(const uint32_t *px, int w, int h);
extern void xbox_InputSetBlocked(int blocked);
extern void mkb_set_aim(double sens, int invert_y, double deadzone);
extern void mkb_set_active(int on);

/* What the settings change while the game runs. */
void dw_apply_live(void)
{
    dwvk_set_display_options(g_dw.vsync, g_dw.fps_cap, g_dw.aspect);
    if (g_dw.fullscreen != s_fullscreen)
        dw_set_fullscreen(g_dw.fullscreen);
    mkb_set_aim(g_dw.mouse_sens / 100.0, g_dw.invert_y, -1.0);
    mkb_set_active(g_dw.mkb);
    dw_settings_apply_input();
    xa2_set_volume(g_dw.volume / 100.0f);
    if (!g_dw.show_fps)
        SetWindowTextA(g_dw_hwnd, "Darkwatch");
}

static void overlay_open(void)
{
    static uint32_t frame[640 * 480];
    uint32_t va = s_last_fb_va, pitch = s_last_fb_pitch, y, x;
    const uint32_t *bd = NULL;
    if (va && pitch >= 640 * 2) {
        const uint8_t *src = (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
        for (y = 0; y < 480; y++) {
            const uint8_t *row = src + (size_t)y * pitch;
            uint32_t *dst = frame + y * 640;
            if (pitch / 640 >= 4)
                for (x = 0; x < 640; x++)
                    dst[x] = ((const uint32_t *)row)[x] | 0xFF000000u;
            else
                for (x = 0; x < 640; x++) {
                    uint16_t v = ((const uint16_t *)row)[x];
                    dst[x] = 0xFF000000u | ((uint32_t)((v >> 11) & 31) * 255 / 31) << 16
                           | ((uint32_t)((v >> 5) & 63) * 255 / 63) << 8 | (uint32_t)(v & 31) * 255 / 31;
                }
        }
        bd = frame;
    }
    xbox_InputSetBlocked(1);
    xbox_InputKeysClear();
    ClipCursor(NULL);
    s_overlay = 1;
    launcher_open_overlay(g_dw_hwnd, bd);
    fprintf(stderr, "[PC] settings overlay opened\n");
}

static void overlay_close(void)
{
    launcher_close();
    s_overlay = 0;
    xbox_InputSetBlocked(0);
    dw_apply_live();
    dw_settings_save();
    fprintf(stderr, "[PC] settings overlay closed\n");
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (!s_game_running || s_overlay) {
        LRESULT r;
        if (launcher_wndproc(h, m, w, l, &r))
            return r;
    }
    switch (m) {
    case WM_CLOSE:
        if (s_game_running) {
            /* The title has no clean shutdown; its saves are written as it
             * goes. Keep the window settings and leave. */
            dw_settings_save();
            fflush(stderr);
            ExitProcess(0);
        }
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (!s_fullscreen && w == SIZE_RESTORED) {
            g_dw.win_w = LOWORD(l);
            g_dw.win_h = HIWORD(l);
        }
        dwvk_window_resized();
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {          /* Alt+Enter */
            dw_set_fullscreen(!s_fullscreen);
            return 0;
        }
        if (w == VK_F10)
            return 0;                                    /* no menu-bar focus */
        break;
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {
            dw_set_fullscreen(!s_fullscreen);
            return 0;
        }
        if (w == VK_F1 && !(l & (1 << 30)) && s_game_running && !s_overlay) {
            overlay_open();
            return 0;
        }
        if (w == VK_F12 && !(l & (1 << 30)) && s_game_running && !s_overlay) {
            /* A screenshot with the frame's draw list beside it (for bug
             * reports): screenshots\shot-<date>-<time>.png / .txt. */
            char dir[DW_PATH_MAX + 32], prefix[DW_PATH_MAX + 64];
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(dir, sizeof dir, "%s\\screenshots", dw_exe_dir());
            CreateDirectoryA(dir, NULL);
            snprintf(prefix, sizeof prefix, "%s\\shot-%04u%02u%02u-%02u%02u%02u", dir,
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            nv2a_capture_request(prefix);
            fprintf(stderr, "[PC] screenshot: %s\n", prefix);
            return 0;
        }
        xbox_InputKeySet((int)w, 1);
        return 0;
    case WM_KEYUP:
        xbox_InputKeySet((int)w, 0);
        return 0;
    case WM_SYSCHAR:
        return 0;                                        /* no beep on Alt+key */
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            xbox_InputKeysClear();
            if (g_dw.mute_unfocused && s_game_running)
                xa2_set_volume(0.0f);
        } else if (g_dw.mute_unfocused && s_game_running) {
            xa2_set_volume(g_dw.volume / 100.0f);
        }
        cursor_update();
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && s_game_running && !s_overlay
            && (mkb_capture_wanted() || s_fullscreen)) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (s_game_running && !dwvk_presenting())
            gdi_paint(dc);
        else if (s_game_running)
            ;                                            /* Vulkan owns it */
        EndPaint(h, &ps);
        return 0;
    }
    case 0x02E0: /* WM_DPICHANGED: take the size Windows suggests for the new monitor */
        if (!s_fullscreen) {
            const RECT *r = (const RECT *)l;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
        case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 480;
        mm->ptMinTrackSize.y = 360;
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND window_create(void)
{
    WNDCLASSEXA wc;
    RECT wa, r;
    int ch, cw;
    HWND h;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "DarkwatchRecompiled";
    RegisterClassExA(&wc);

    /* Windowed: what the settings remember, else 4:3 at 80% of the work
     * area's height. */
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    ch = g_dw.win_h >= 360 ? g_dw.win_h : (wa.bottom - wa.top) * 4 / 5;
    cw = g_dw.win_w >= 480 ? g_dw.win_w : ch * 4 / 3;
    if (cw < 1100 && !g_dw.win_w)                       /* room for the launcher */
        cw = 1100 < (wa.right - wa.left) ? 1100 : wa.right - wa.left;
    SetRect(&r, 0, 0, cw, ch);
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    h = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, "Darkwatch", WS_OVERLAPPEDWINDOW,
                        wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2,
                        wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    return h;
}

/* ── the framebuffer window, as the runtime sees it ──────────────── */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars) { (void)name; (void)max_chars; }
int  xbox_FramebufferDumpBmp(const char *path) { (void)path; return -1; }

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_draws, (LONG)draws);
}

/* The executor's flip, on its thread: the frame it has just finished. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    uint32_t bpp, y, x;
    LONG next;

    if (!fb_va || !pitch)
        return;
    s_last_fb_va = fb_va;
    s_last_fb_pitch = pitch;
    if (dwvk_present(fb_va, pitch, 480))
        return;
    if (dwvk_presenting())
        return;                                 /* minimised, or a format it skips */
    /* No Vulkan display: copy the frame out for GDI. */
    next = s_gdi_idx == 0 ? 1 : 0;
    if (!s_gdi_frame[next] && !(s_gdi_frame[next] = calloc(640 * 480, 4)))
        return;
    bpp = pitch / 640;
    src = (const uint8_t *)((uintptr_t)fb_va + g_xbox_mem_offset);
    for (y = 0; y < 480; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_gdi_frame[next] + (size_t)y * 640;
        if (bpp == 4)
            memcpy(dst, row, 640 * 4);
        else if (bpp == 2)
            for (x = 0; x < 640; x++) {
                uint16_t v = ((const uint16_t *)row)[x];
                dst[x] = ((uint32_t)((v >> 11) & 31) * 255 / 31) << 16
                       | ((uint32_t)((v >> 5) & 63) * 255 / 63) << 8
                       | (uint32_t)(v & 31) * 255 / 31;
            }
    }
    InterlockedExchange(&s_gdi_idx, next);
    InvalidateRect(g_dw_hwnd, NULL, FALSE);
}

/* The D3D device's pushbuffer write pointer (device+0), for the walker's
 * desync reports. */
uint32_t nv2a_pb_title_write_ptr(void)
{
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + DW_D3D_DEVICE_PTR);
    if (!dev) return 0;
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + dev);
}

/* ── Vulkan surface ──────────────────────────────────────────────── */

static VkSurfaceKHR make_win32_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa,
                                       VkPhysicalDevice phys, VkExtent2D *extent)
{
    PFN_vkCreateWin32SurfaceKHR f =
        (PFN_vkCreateWin32SurfaceKHR)gipa(inst, "vkCreateWin32SurfaceKHR");
    VkWin32SurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    RECT r;

    (void)phys;
    ci.hinstance = GetModuleHandleA(NULL);
    ci.hwnd = g_dw_hwnd;
    if (!f || f(inst, &ci, NULL, &surf) != VK_SUCCESS) {
        fprintf(stderr, "[PC] vkCreateWin32SurfaceKHR failed\n");
        return VK_NULL_HANDLE;
    }
    GetClientRect(g_dw_hwnd, &r);
    extent->width = (uint32_t)r.right;
    extent->height = (uint32_t)r.bottom;
    return surf;
}

/* ── D3D fence semaphore (see main_posix.c) ──────────────────────── */

static DWORD WINAPI semaphore_watcher(LPVOID arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 6000; i++) {          /* up to 60 s */
        uint32_t dev = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + DW_D3D_DEVICE_PTR);
        if (dev) {
            uint32_t sem = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + dev + 0x30);
            if (sem) {
                nv2a_pb_set_semaphore_target(sem);
                fprintf(stderr, "[BOOT] D3D device 0x%08X: fence semaphore at "
                                "0x%08X registered with the executor\n", dev, sem);
                return 0;
            }
        }
        Sleep(10);
    }
    fprintf(stderr, "[BOOT] D3D device never appeared; fence semaphore not registered\n");
    return 0;
}

/* ── sampling profiler (DW_PROFILE=1), as on the PS5 ─────────────── */

static volatile uint32_t *s_main_esp;
static HANDLE s_main_thread;
extern volatile int g_dw_in_kick_wait;

#define PROF_SLOTS 4096
static struct { uint32_t va[3], n; } s_prof[PROF_SLOTS];

static int is_code(uint32_t v)
{
    return v > g_xbox_code_lo && v < g_xbox_code_hi
        && !(v >= 0x00354000u && v < 0x00357020u);
}

static int prof_cmp(const void *a, const void *b)
{
    const uint32_t *x = a, *y = b;
    return (int)y[3] - (int)x[3];
}

static DWORD WINAPI profiler_thread(LPVOID arg)
{
    unsigned ticks = 0, run = 0, sleep = 0, kick = 0;
    ULONG64 last_cyc = 0;
    (void)arg;
    for (;;) {
        ULONG64 cyc = 0;
        int running;
        Sleep(5);
        QueryThreadCycleTime(s_main_thread, &cyc);
        running = cyc - last_cyc > 2000000ull;          /* ~0.5 ms of 5 */
        last_cyc = cyc;
        if (g_dw_in_kick_wait)
            kick++;
        else if (running) run++; else sleep++;
        if (s_main_esp && !g_dw_in_kick_wait) {
            uint32_t esp = *s_main_esp, shown = 0, va[3] = { 0, 0, 0 };
            if (esp > 0x10000 && esp < 0x04000000) {
                const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp);
                int i;
                for (i = 0; i < 256 && shown < 3; i++)
                    if (is_code(sp[i]))
                        va[shown++] = sp[i];
                if (shown) {
                    unsigned h = ((va[0] * 2654435761u) ^ (va[1] * 40503u) ^ va[2]) & (PROF_SLOTS - 1), k;
                    for (k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1))
                        if (!s_prof[h].va[0] || !memcmp(s_prof[h].va, va, 12)) {
                            memcpy(s_prof[h].va, va, 12);
                            s_prof[h].n++;
                            break;
                        }
                }
            }
        }
        if (++ticks % 4000 == 0) {                         /* 20 s */
            static uint32_t rows[PROF_SLOTS][4];
            unsigned total = run + sleep + kick;
            int n = 0, i;
            for (i = 0; i < PROF_SLOTS; i++)
                if (s_prof[i].va[0]) {
                    memcpy(rows[n], s_prof[i].va, 12);
                    rows[n][3] = s_prof[i].n;
                    n++;
                }
            qsort(rows, (size_t)n, sizeof rows[0], prof_cmp);
            if (total) {
                fprintf(stderr, "[PROF] main thread, 20 s: %u%% in the GPU kick wait, %u%% running, "
                        "%u%% asleep. Stacks (innermost first):\n",
                        kick * 100 / total, run * 100 / total, sleep * 100 / total);
                for (i = 0; i < n && i < 16; i++)
                    fprintf(stderr, "[PROF]   %08X < %08X < %08X  %4.1f%%\n", rows[i][0], rows[i][1],
                            rows[i][2], 100.0 * rows[i][3] / total);
            }
            memset(s_prof, 0, sizeof s_prof);
            run = sleep = kick = 0;
        }
    }
    return 0;
}

/* ── boot ────────────────────────────────────────────────────────── */

static void low4g_release(void);
extern int dw_cpu_ok(void);
static void *s_xbe;
static size_t s_xbe_size;

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *out_size = (size_t)size;
    return data;
}

static DWORD WINAPI game_thread(LPVOID arg)
{
    (void)arg;
    g_esp = XBOX_STACK_TOP;
    s_main_esp = &g_esp;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    xbox_ThreadRegister("main", &g_esp, &g_eax, &g_ecx, &g_edx, &g_ebx, &g_esi, &g_edi);
    xbox_WatchdogStart();
    if (getenv("DW_PROFILE") && atoi(getenv("DW_PROFILE")) != 0)
        CloseHandle(CreateThread(NULL, 0, profiler_thread, NULL, 0, NULL));
    fprintf(stderr, "[BOOT] entry 0x%08X, esp 0x%08X. Starting.\n", DW_ENTRY_POINT, g_esp);
    xbe_entry_point();
    fprintf(stderr, "[BOOT] the game returned\n");
    fflush(stderr);
    PostMessageA(g_dw_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

static int boot(char *err, size_t errlen)
{
    char xbe_path[DW_PATH_MAX + 16];
    const char *game_dir = getenv("DW_GAME_DIR");
    const char *save_dir = getenv("DW_SAVE_DIR");
    long map_mb = 64;
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    fprintf(stderr, "[PC] Darkwatch PC, built %s %s; mouse sensitivity %d%%%s\n", __DATE__, __TIME__,
            g_dw.mouse_sens, g_dw.invert_y ? ", inverted" : "");
    fprintf(stderr, "[PC] display %d px high at %d Hz; settings: %s, %s, interpolation %s, "
            "V-Sync %s, frame cap %d, internal resolution %s\n",
            dw_display_height(), dw_display_refresh(), g_dw.fullscreen ? "fullscreen" : "windowed",
            g_dw.aspect ? "stretch" : "4:3", g_dw.interp ? "on" : "off", g_dw.vsync ? "on" : "off",
            g_dw.fps_cap, getenv("DW_RES_SCALE") ? getenv("DW_RES_SCALE") : "?");
    fprintf(stderr, "[BOOT] %lu CPU(s); game data %s; saves %s\n",
            (unsigned long)si.dwNumberOfProcessors, game_dir, save_dir);
    CreateDirectoryA(save_dir, NULL);
    snprintf(xbe_path, sizeof xbe_path, "%s\\default.xbe", game_dir);
    s_xbe = load_file(xbe_path, &s_xbe_size);
    if (!s_xbe) {
        snprintf(err, errlen, "Cannot read %s.\n\nPoint Settings > General > Game folder at "
                 "the folder with Darkwatch's default.xbe and .pck files.", xbe_path);
        return 0;
    }
    low4g_release();
    {
        const char *e = getenv("RECOMP_MAP_MB");
        long mb = e && *e ? atol(e) : 128;
        if (mb > 64 && mb <= 256) {
            xbox_SetMapSize((size_t)mb << 20);
            map_mb = mb;
        }
    }
    if (!xbox_MemoryLayoutInit(s_xbe, s_xbe_size)) {
        int ok = 0;
        if (map_mb > 64) {
            fprintf(stderr, "[BOOT] a %ld MB guest map failed; retrying with 64 MB\n", map_mb);
            xbox_MemoryLayoutShutdown();
            xbox_SetMapSize(0);
            map_mb = 64;
            ok = xbox_MemoryLayoutInit(s_xbe, s_xbe_size);
        }
        if (!ok) {
            snprintf(err, errlen, "Could not reserve the Xbox's address space "
                     "(the low 2 GB of the process).\n\nClose other programs that inject "
                     "into games (overlays, recorders) and try again.");
            return 0;
        }
    }
    fprintf(stderr, "[BOOT] guest map %ld MB\n", map_mb);
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
    }
    xbox_kernel_init();
    xbox_path_init(game_dir, save_dir);
    xbox_kernel_bridge_init();
    xbox_OhciInit();
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");

    {
        static const char *const exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                            VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        dwvk_set_display_options(g_dw.vsync, g_dw.fps_cap, g_dw.aspect);
        dwvk_set_present(exts, 2, make_win32_surface);
        if (!dwvk_register())
            fprintf(stderr, "[BOOT] no Vulkan: the CPU rasteriser draws, shown with GDI "
                            "(slow; update the graphics driver)\n");
    }
    CloseHandle(CreateThread(NULL, 0, semaphore_watcher, NULL, 0, NULL));
    return 1;
}

/* ── the guest's address space, held until boot ──────────────────── */

/* The game's memory lives at fixed addresses in the low 4 GB (RAM from
 * 0x10000, its mirrors, the 0x80000000 window, the 0xF0000000-0xFFFFFFFF
 * apertures). The launcher allocates before boot -- images, fonts, the
 * window's bitmap, a Vulkan instance, the shell's folder picker -- and any of
 * that landing down there would leave boot unable to map the guest. So the
 * free low 4 GB is reserved first thing and handed back just before the
 * memory layout claims it. */
static void *s_low[2048];
static int s_nlow;

static void low4g_reserve(void)
{
    uintptr_t a = 0x10000;
    while (a < 0x100000000ull && s_nlow < 2048) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t base, end;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi))
            break;
        base = (uintptr_t)mbi.BaseAddress;
        end = base + mbi.RegionSize;
        if (end > 0x100000000ull)
            end = 0x100000000ull;
        if (mbi.State == MEM_FREE) {
            uintptr_t b = (base + 0xFFFF) & ~(uintptr_t)0xFFFF, e = end & ~(uintptr_t)0xFFFF;
            if (e > b) {
                void *p = VirtualAlloc((void *)b, e - b, MEM_RESERVE, PAGE_NOACCESS);
                if (p)
                    s_low[s_nlow++] = p;
            }
        }
        a = end > a ? end : a + 0x10000;
    }
}

static void low4g_release(void)
{
    int i;
    for (i = 0; i < s_nlow; i++)
        VirtualFree(s_low[i], 0, MEM_RELEASE);
    s_nlow = 0;
}

/* ── main ────────────────────────────────────────────────────────── */

static void dpi_aware(void)
{
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleA("user32.dll");
    SetCtx f = u ? (SetCtx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!f || !f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */))
        SetProcessDPIAware();
}

/* Keep the game's threads out of Windows' power throttling (EcoQoS), which
 * parks a background-looking process on slow cores. */
static void no_throttle(void)
{
    typedef struct { ULONG Version, ControlMask, StateMask; } PT;
    typedef BOOL (WINAPI *SetPI)(HANDLE, int, LPVOID, DWORD);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    SetPI f = k ? (SetPI)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;
    PT pt = { 1, 0x1 /* EXECUTION_SPEED */, 0 };
    if (f)
        f(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof pt);
}

int main(int argc, char **argv)
{
    int play_now = 0, i;
    MSG msg;
    char err[1024] = "";

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--play") || !strcmp(argv[i], "-p"))
            play_now = 1;

    low4g_reserve();
    dpi_aware();
    timeBeginPeriod(1);
    no_throttle();
    dw_settings_load();
    log_open();
    fprintf(stderr, "=== Darkwatch - static recompilation (Windows) ===\n");
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(crash_filter);

    g_dw_hwnd = window_create();
    if (!g_dw_hwnd) {
        MessageBoxA(NULL, "Could not create the window.", "Darkwatch", MB_ICONERROR);
        return 1;
    }

    for (;;) {
        /* Shift held at start shows the launcher even when it is skipped. */
        int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!(play_now || (g_dw.skip_launcher && !shift)) || err[0]) {
            /* The launcher, in the window, until Play or Quit. */
            ShowWindow(g_dw_hwnd, SW_SHOW);
            launcher_open(g_dw_hwnd, err[0] ? err : NULL);
            err[0] = 0;
            for (;;) {
                int r;
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        dw_settings_save();
                        return 0;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                r = launcher_tick();
                if (r == LAUNCHER_QUIT) {
                    dw_settings_save();
                    return 0;
                }
                if (r == LAUNCHER_PLAY)
                    break;
                MsgWaitForMultipleObjects(0, NULL, FALSE, 8, QS_ALLINPUT);
            }
            launcher_close();
        } else {
            err[0] = 0;
        }
        dw_settings_save();
        if (g_dw.fullscreen)
            dw_set_fullscreen(1);
        ShowWindow(g_dw_hwnd, SW_SHOW);
        UpdateWindow(g_dw_hwnd);
        dw_settings_apply_env(dw_display_height());
        dw_settings_apply_input();
        if (boot(err, sizeof err))
            break;
        fprintf(stderr, "[BOOT] %s\n", err);
        play_now = 0;
        if (s_xbe) {
            /* Past the point of no return (guest memory is mapped). */
            MessageBoxA(g_dw_hwnd, err, "Darkwatch", MB_ICONERROR);
            return 1;
        }
    }

    {
        /* The game: a desktop-sized stack (lifted code recurses on it). */
        HANDLE th;
        InterlockedExchange(&s_game_running, 1);
        th = CreateThread(NULL, 64u << 20, game_thread, NULL,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!th) {
            MessageBoxA(g_dw_hwnd, "Could not start the game thread.", "Darkwatch", MB_ICONERROR);
            return 1;
        }
        SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(th);
    }

    /* The window's messages for as long as the game runs; the title bar shows
     * the frame rates once a second when asked. */
    {
        DWORD t0 = GetTickCount();
        LONG f0 = 0;
        unsigned d0 = 0;
        for (;;) {
            DWORD now;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    dw_settings_save();
                    fflush(stderr);
                    ExitProcess(0);
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (s_overlay) {
                int r = launcher_tick();
                if (r == LAUNCHER_RESUME)
                    overlay_close();
                else if (r == LAUNCHER_QUIT) {
                    dw_settings_save();
                    fflush(stderr);
                    ExitProcess(0);
                }
            }
            cursor_update();
            now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                unsigned d = dwvk_display_frames();
                if (g_dw.show_fps) {
                    char t[160];
                    snprintf(t, sizeof t, "Darkwatch  |  %.0f fps shown, game %.0f fps  |  %ld draws",
                             (d - d0) * 1000.0 / (now - t0), (f - f0) * 1000.0 / (now - t0),
                             (long)s_draws);
                    SetWindowTextA(g_dw_hwnd, t);
                }
                t0 = now; f0 = f; d0 = d;
            }
            MsgWaitForMultipleObjects(0, NULL, FALSE, s_overlay ? 8 : 50, QS_ALLINPUT);
        }
    }
    return 0;
}
