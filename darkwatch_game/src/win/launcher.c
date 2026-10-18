/*
 * launcher.c -- the launcher's Windows side: a DIB the screens are drawn
 * into, the window's mouse and keyboard, XInput for the controller, the
 * folder picker, and the settings file. The screens themselves are
 * launcher_ui.c.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

#include "launcher.h"
#include "launcher_ui.h"
#include "dw_settings.h"
#include "win_host.h"

extern void dw_apply_live(void);

extern int dwvk_list_devices(char names[][256], int max);
extern int dwvk_presenting(void);
extern void dwvk_overlay_set(const uint32_t *px, int w, int h);
extern int dwvk_overlay_present(void);

static HWND s_hwnd;
static int s_open;
static HDC s_memdc;
static HBITMAP s_dib, s_olddib;
static uint32_t *s_bits;
static int s_w, s_h;
static int s_dirty = 1;
static LuiInput s_in;                   /* events gathered since the last frame */
static LuiEnv s_env;
static char s_notice[1024];
static char s_version[96];
static char s_log[DW_PATH_MAX + 32];
static LARGE_INTEGER s_qpf, s_qp0;
static int s_result;

/* ── resources ───────────────────────────────────────────────────── */

static const void *rc_data(int id, int *len)
{
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
    HGLOBAL g;
    if (!r || !(g = LoadResource(NULL, r)))
        return NULL;
    *len = (int)SizeofResource(NULL, r);
    return LockResource(g);
}

static void ensure_dib(void)
{
    RECT r;
    BITMAPINFO bi;
    HDC dc;
    GetClientRect(s_hwnd, &r);
    if (r.right < 1 || r.bottom < 1)
        return;
    if (s_dib && r.right == s_w && r.bottom == s_h)
        return;
    if (s_dib) {
        SelectObject(s_memdc, s_olddib);
        DeleteObject(s_dib);
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = r.right;
    bi.bmiHeader.biHeight = -r.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc = GetDC(s_hwnd);
    if (!s_memdc)
        s_memdc = CreateCompatibleDC(dc);
    s_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
    ReleaseDC(s_hwnd, dc);
    s_olddib = (HBITMAP)SelectObject(s_memdc, s_dib);
    s_w = r.right;
    s_h = r.bottom;
    s_dirty = 1;
}

static void env_refresh(void)
{
    XINPUT_STATE st;
    DWORD i;
    s_env.display_h = dw_display_height();
    s_env.display_refresh = dw_display_refresh();
    {
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoA(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            s_env.display_w = mi.rcMonitor.right - mi.rcMonitor.left;
    }
    s_env.fullscreen = dw_is_fullscreen();
    s_env.pad_connected = 0;
    for (i = 0; i < 4; i++)
        if (XInputGetState(i, &st) == ERROR_SUCCESS)
            s_env.pad_connected = 1;
}

static void first_init(void)
{
    static int inited;
    if (!inited) {
        int n1 = 0, n2 = 0, n3 = 0, n4 = 0;
        const void *f1 = rc_data(101, &n1), *f2 = rc_data(102, &n2);
        const void *f3 = rc_data(103, &n3), *f4 = rc_data(104, &n4);
        char a[DW_PATH_MAX + 64], b[DW_PATH_MAX + 64], e[DW_PATH_MAX + 64], w[DW_PATH_MAX + 64];
        lui_init(f1, n1, f2, n2, f3, n3, f4, n4);
        snprintf(a, sizeof a, "%s\\assets\\launcher_bg.png", dw_exe_dir());
        snprintf(b, sizeof b, "%s\\assets\\launcher_bg_blur.png", dw_exe_dir());
        snprintf(e, sizeof e, "%s\\assets\\launcher_emblem.png", dw_exe_dir());
        snprintf(w, sizeof w, "%s\\assets\\launcher_wordmark.png", dw_exe_dir());
        lui_load_art(a, b, e, w);
        s_env.ngpu = dwvk_list_devices(s_env.gpu, 8);
        snprintf(s_version, sizeof s_version, "Darkwatch PC 1.0  (built %s %.5s)", __DATE__, __TIME__);
        snprintf(s_log, sizeof s_log, "%s\\darkwatch-log.txt", dw_exe_dir());
        s_env.version = s_version;
        s_env.log_path = s_log;
        inited = 1;
    }
}

void launcher_open(HWND hwnd, const char *err)
{
    s_hwnd = hwnd;
    s_open = 1;
    s_result = LAUNCHER_NONE;
    s_env.in_game = 0;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qp0);
    first_init();
    snprintf(s_notice, sizeof s_notice, "%s", err ? err : "");
    s_env.notice = s_notice[0] ? s_notice : NULL;
    if (g_dw.fullscreen != dw_is_fullscreen())
        dw_set_fullscreen(g_dw.fullscreen);
    memset(&s_in, 0, sizeof s_in);
    s_in.cap_pad = -1;
    s_in.mx = s_in.my = -10000;
    env_refresh();
    ensure_dib();
    s_dirty = 1;
    SetWindowTextA(hwnd, "Darkwatch");
}

void launcher_open_overlay(HWND hwnd, const uint32_t *backdrop)
{
    s_hwnd = hwnd;
    s_open = 1;
    s_result = LAUNCHER_NONE;
    s_env.in_game = 1;
    s_env.notice = NULL;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qp0);
    first_init();
    lui_set_backdrop(backdrop, 640, 480);
    lui_goto(LUI_SCREEN_SETTINGS, -1);
    memset(&s_in, 0, sizeof s_in);
    s_in.cap_pad = -1;
    s_in.mx = s_in.my = -10000;
    env_refresh();
    ensure_dib();
    s_dirty = 1;
}

void launcher_close(void)
{
    s_open = 0;
    if (s_env.in_game) {
        dwvk_overlay_set(NULL, 0, 0);
        s_env.in_game = 0;
    }
    /* the loading picture stays until the game's swapchain paints over it */
}

/* ── the controller ──────────────────────────────────────────────── */

/* Physical controls in pad-control order (A B X Y LB RB LT RT, D-pad up
 * down left right, Start Back LS RS), as booleans. */
static void pad_read(int out[16], int *connected)
{
    XINPUT_STATE st;
    DWORD i;
    memset(out, 0, 16 * sizeof(int));
    *connected = 0;
    for (i = 0; i < 4; i++) {
        WORD w;
        if (XInputGetState(i, &st) != ERROR_SUCCESS)
            continue;
        *connected = 1;
        w = st.Gamepad.wButtons;
        out[0] |= !!(w & XINPUT_GAMEPAD_A);
        out[1] |= !!(w & XINPUT_GAMEPAD_B);
        out[2] |= !!(w & XINPUT_GAMEPAD_X);
        out[3] |= !!(w & XINPUT_GAMEPAD_Y);
        out[4] |= !!(w & XINPUT_GAMEPAD_LEFT_SHOULDER);
        out[5] |= !!(w & XINPUT_GAMEPAD_RIGHT_SHOULDER);
        out[6] |= st.Gamepad.bLeftTrigger > 128;
        out[7] |= st.Gamepad.bRightTrigger > 128;
        out[8] |= !!(w & XINPUT_GAMEPAD_DPAD_UP) || st.Gamepad.sThumbLY > 20000;
        out[9] |= !!(w & XINPUT_GAMEPAD_DPAD_DOWN) || st.Gamepad.sThumbLY < -20000;
        out[10] |= !!(w & XINPUT_GAMEPAD_DPAD_LEFT) || st.Gamepad.sThumbLX < -20000;
        out[11] |= !!(w & XINPUT_GAMEPAD_DPAD_RIGHT) || st.Gamepad.sThumbLX > 20000;
        out[12] |= !!(w & XINPUT_GAMEPAD_START);
        out[13] |= !!(w & XINPUT_GAMEPAD_BACK);
        out[14] |= !!(w & XINPUT_GAMEPAD_LEFT_THUMB);
        out[15] |= !!(w & XINPUT_GAMEPAD_RIGHT_THUMB);
    }
}

static void pad_poll(double now)
{
    static int prev[16];
    static double next_repeat[16];
    int cur[16], conn, i;
    static DWORD last_poll;
    DWORD t = GetTickCount();
    if (t - last_poll < 8)
        return;
    last_poll = t;
    pad_read(cur, &conn);
    if (conn != s_env.pad_connected) {
        s_env.pad_connected = conn;
        s_dirty = 1;
    }
    for (i = 0; i < 16; i++) {
        int press = cur[i] && !prev[i], rep = 0;
        if (press)
            next_repeat[i] = now + 0.38;
        else if (cur[i] && i >= 8 && i <= 11 && now >= next_repeat[i]) {
            rep = 1;
            next_repeat[i] = now + 0.085;
        }
        prev[i] = cur[i];
        if (!press && !rep)
            continue;
        s_dirty = 1;
        if (lui_capture_mode() == 2) {
            /* the stick's directions do not bind; only real controls */
            if (press && s_in.cap_pad < 0)
                s_in.cap_pad = i;
            continue;
        }
        if (lui_capture_mode() == 1)
            continue;
        switch (i) {
        case 0: s_in.ok = 1; break;
        case 1: s_in.back = 1; break;
        case 4: s_in.page_prev = 1; break;
        case 5: s_in.page_next = 1; break;
        case 8: s_in.up = 1; break;
        case 9: s_in.down = 1; break;
        case 10: s_in.left = 1; break;
        case 11: s_in.right = 1; break;
        case 12: s_in.start = 1; break;
        }
    }
}

/* ── window messages ─────────────────────────────────────────────── */

/* The virtual key of a key message, with the left/right modifiers told
 * apart (as the bindings name them). */
static int key_vk(WPARAM w, LPARAM l)
{
    int vk = (int)w, sc = (int)((l >> 16) & 0xFF), ext = (int)((l >> 24) & 1);
    if (vk == VK_SHIFT)
        return sc == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL)
        return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)
        return ext ? VK_RMENU : VK_LMENU;
    return vk;
}

static int capture_mouse(int in)
{
    if (lui_capture_mode() == 1) {
        if (!s_in.cap_input)
            s_in.cap_input = in;
        s_dirty = 1;
        return 1;
    }
    return 0;
}

int launcher_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result)
{
    if (!s_open)
        return 0;
    switch (m) {
    case WM_SIZE:
        ensure_dib();
        s_dirty = 1;
        return 0;                           /* fall through to the host as well */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (s_env.in_game && dwvk_presenting())
            s_dirty = 1;                    /* the swapchain shows it: present again */
        else if (s_dib)
            BitBlt(dc, 0, 0, s_w, s_h, s_memdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        *result = 0;
        return 1;
    }
    case WM_ERASEBKGND:
        *result = 1;
        return 1;
    case WM_MOUSEMOVE:
        s_in.mx = GET_X_LPARAM(l);
        s_in.my = GET_Y_LPARAM(l);
        s_in.mouse_moved = 1;
        s_dirty = 1;
        *result = 0;
        return 1;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        if (!capture_mouse(MKB_IN_MOUSE1)) {
            s_in.mdown = 1;
            s_in.mpress = 1;
        }
        s_in.mx = GET_X_LPARAM(l);
        s_in.my = GET_Y_LPARAM(l);
        s_dirty = 1;
        *result = 0;
        return 1;
    case WM_LBUTTONUP:
        ReleaseCapture();
        s_in.mdown = 0;
        s_in.mrelease = 1;
        s_in.mx = GET_X_LPARAM(l);
        s_in.my = GET_Y_LPARAM(l);
        s_dirty = 1;
        *result = 0;
        return 1;
    case WM_RBUTTONDOWN:
        capture_mouse(MKB_IN_MOUSE2);
        *result = 0;
        return 1;
    case WM_MBUTTONDOWN:
        capture_mouse(MKB_IN_MOUSE3);
        *result = 0;
        return 1;
    case WM_XBUTTONDOWN:
        capture_mouse(GET_XBUTTON_WPARAM(w) == XBUTTON1 ? MKB_IN_MOUSE4 : MKB_IN_MOUSE5);
        *result = TRUE;
        return 1;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        if (!capture_mouse(d > 0 ? MKB_IN_WHEELUP : MKB_IN_WHEELDOWN)) {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &p);
            s_in.mx = p.x;
            s_in.my = p.y;
            s_in.wheel += d / WHEEL_DELTA ? d / WHEEL_DELTA : (d > 0 ? 1 : -1);
        }
        s_dirty = 1;
        *result = 0;
        return 1;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int vk = key_vk(w, l), repeat = (l >> 30) & 1;
        if (lui_capture_mode() == 1) {
            if (repeat)
                ;
            else if (vk == VK_ESCAPE)
                s_in.cap_cancel = 1;
            else if (vk == VK_DELETE)
                s_in.cap_clear = 1;
            else if (vk != VK_F9 && vk != VK_F11 && !s_in.cap_input)
                s_in.cap_input = vk;
            s_dirty = 1;
            *result = 0;
            return 1;
        }
        if (lui_capture_mode() == 2) {
            if (vk == VK_ESCAPE)
                s_in.cap_cancel = 1;
            s_dirty = 1;
            *result = 0;
            return 1;
        }
        if (m == WM_SYSKEYDOWN)
            return 0;                       /* Alt+Enter, Alt+F4: the host's */
        if (vk == VK_F1 && s_env.in_game && !repeat) {
            s_result = LAUNCHER_RESUME;
            *result = 0;
            return 1;
        }
        switch (vk) {
        case VK_UP: s_in.up = 1; break;
        case VK_DOWN: s_in.down = 1; break;
        case VK_LEFT: s_in.left = 1; break;
        case VK_RIGHT: s_in.right = 1; break;
        case VK_RETURN: case VK_SPACE: s_in.ok = 1; break;
        case VK_ESCAPE: case VK_BACK: s_in.back = 1; break;
        case VK_PRIOR: s_in.page_prev = 1; break;
        case VK_NEXT: case VK_TAB: s_in.page_next = 1; break;
        default: return 0;                  /* F11 and the rest: the host's */
        }
        s_dirty = 1;
        *result = 0;
        return 1;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        *result = 0;
        return 1;
    }
    return 0;
}

/* ── folder picker ───────────────────────────────────────────────── */

static int pick_folder(char *out, size_t len, const char *title, const char *start)
{
    BROWSEINFOW bi;
    WCHAR wtitle[128], path[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    int ok = 0;
    (void)start;
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 128);
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = s_hwnd;
    bi.lpszTitle = wtitle;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)len, NULL, NULL);
            ok = 1;
        }
        CoTaskMemFree(pidl);
    }
    return ok;
}

/* ── the tick ────────────────────────────────────────────────────── */

static void draw(void)
{
    UiCanvas c;
    LARGE_INTEGER now;
    int r;
    if (!s_bits)
        return;
    QueryPerformanceCounter(&now);
    s_in.time = (double)(now.QuadPart - s_qp0.QuadPart) / (double)s_qpf.QuadPart;
    s_env.fullscreen = dw_is_fullscreen();
    ui_canvas_init(&c, s_bits, s_w, s_h);
    GdiFlush();
    r = lui_frame(&c, &s_in, &s_env);
    /* events are consumed by the frame; the button and pointer state stay */
    {
        int mx = s_in.mx, my = s_in.my, md = s_in.mdown;
        memset(&s_in, 0, sizeof s_in);
        s_in.mx = mx;
        s_in.my = my;
        s_in.mdown = md;
        s_in.cap_pad = -1;
    }
    if (s_env.in_game && dwvk_presenting()) {
        /* over the game: through the swapchain */
        dwvk_overlay_set(s_bits, s_w, s_h);
        dwvk_overlay_present();
    } else {
        HDC dc = GetDC(s_hwnd);
        BitBlt(dc, 0, 0, s_w, s_h, s_memdc, 0, 0, SRCCOPY);
        ReleaseDC(s_hwnd, dc);
    }
    switch (r) {
    case LUI_RESUME:
        s_result = LAUNCHER_RESUME;
        break;
    case LUI_PLAY:
        s_result = LAUNCHER_PLAY;
        ui_canvas_init(&c, s_bits, s_w, s_h);
        lui_draw_loading(&c);
        {
            HDC dc = GetDC(s_hwnd);
            BitBlt(dc, 0, 0, s_w, s_h, s_memdc, 0, 0, SRCCOPY);
            ReleaseDC(s_hwnd, dc);
        }
        break;
    case LUI_QUIT:
        s_result = LAUNCHER_QUIT;
        break;
    case LUI_CHANGED:
        dw_settings_save();
        if (g_dw.fullscreen != dw_is_fullscreen())
            dw_set_fullscreen(g_dw.fullscreen);
        if (s_env.in_game)
            dw_apply_live();
        break;
    case LUI_BROWSE_GAME: {
        char p[DW_PATH_MAX];
        if (pick_folder(p, sizeof p, "Choose the folder with Darkwatch's default.xbe and .pck files",
                        dw_game_dir())) {
            snprintf(g_dw.game_dir, sizeof g_dw.game_dir, "%s", p);
            dw_settings_save();
            s_notice[0] = 0;
            s_env.notice = NULL;
            if (!dw_game_dir_ok(p))
                MessageBoxA(s_hwnd, "That folder has no default.xbe.\n\nChoose the folder that holds "
                            "default.xbe and the .pck files from the Darkwatch disc.",
                            "Darkwatch", MB_ICONWARNING);
        }
        s_dirty = 1;
        break;
    }
    case LUI_BROWSE_SAVE: {
        char p[DW_PATH_MAX];
        if (pick_folder(p, sizeof p, "Choose where Darkwatch keeps its saves", dw_save_dir())) {
            snprintf(g_dw.save_dir, sizeof g_dw.save_dir, "%s", p);
            dw_settings_save();
        }
        s_dirty = 1;
        break;
    }
    case LUI_OPEN_SAVE:
        CreateDirectoryA(dw_save_dir(), NULL);
        ShellExecuteA(s_hwnd, "open", dw_save_dir(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case LUI_OPEN_LOG:
        ShellExecuteA(s_hwnd, "open", s_log, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
}

int launcher_tick(void)
{
    LARGE_INTEGER now;
    double t;
    static DWORD last_env;
    if (!s_open)
        return LAUNCHER_NONE;
    QueryPerformanceCounter(&now);
    t = (double)(now.QuadPart - s_qp0.QuadPart) / (double)s_qpf.QuadPart;
    if (GetForegroundWindow() == s_hwnd)
        pad_poll(t);
    if (GetTickCount() - last_env > 1000) {
        last_env = GetTickCount();
        env_refresh();
    }
    ensure_dib();
    if (s_dirty || lui_animating() || lui_capture_mode()) {
        s_dirty = 0;
        draw();
    }
    return s_result;
}
