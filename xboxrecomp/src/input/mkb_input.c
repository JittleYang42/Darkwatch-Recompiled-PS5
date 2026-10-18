/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a Vulkan swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 *
 * Everything else -- keys, mouse buttons, the wheel -- goes through one
 * binding table (input -> pad control), which a host's settings screen
 * replaces (mkb_set_binds) or an ini overrides (bind.KEY=CONTROL).
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- names ------------------------------------------------------------- */

static const struct { const char *name; int code; } s_input_names[] = {
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "ESC", VK_ESCAPE },
    { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "CAPSLOCK", VK_CAPITAL },
    { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT },
    { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL },
    { "LALT", VK_LMENU }, { "RALT", VK_RMENU },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME },
    { "END", VK_END }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
    { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 },
    { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 }, { "F8", VK_F8 },
    { "F10", VK_F10 }, { "F12", VK_F12 },
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 },
    { "NUM3", VK_NUMPAD3 }, { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 },
    { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 }, { "NUM8", VK_NUMPAD8 },
    { "NUM9", VK_NUMPAD9 }, { "NUMENTER", 0 /* not distinct in VK space */ },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "SLASH", VK_OEM_2 },
    { "SEMICOLON", VK_OEM_1 }, { "QUOTE", VK_OEM_7 }, { "LBRACKET", VK_OEM_4 },
    { "RBRACKET", VK_OEM_6 }, { "BACKSLASH", VK_OEM_5 }, { "MINUS", VK_OEM_MINUS },
    { "EQUALS", VK_OEM_PLUS }, { "GRAVE", VK_OEM_3 },
    { "MOUSE1", MKB_IN_MOUSE1 }, { "MOUSE2", MKB_IN_MOUSE2 }, { "MOUSE3", MKB_IN_MOUSE3 },
    { "MOUSE4", MKB_IN_MOUSE4 }, { "MOUSE5", MKB_IN_MOUSE5 },
    { "WHEELUP", MKB_IN_WHEELUP }, { "WHEELDOWN", MKB_IN_WHEELDOWN },
};

static const char *const s_control_names[MKB_C_COUNT] = {
    "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT",
    "DUP", "DDOWN", "DLEFT", "DRIGHT", "START", "BACK", "LS", "RS",
    "LUP", "LDOWN", "LLEFT", "LRIGHT", "RUP", "RDOWN", "RLEFT", "RRIGHT",
};

int mkb_input_from_name(const char *name)
{
    size_t i;
    if (!name || !*name)
        return 0;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    for (i = 0; i < sizeof s_input_names / sizeof s_input_names[0]; i++)
        if (!_stricmp(name, s_input_names[i].name))
            return s_input_names[i].code;
    if ((name[0] == 'V' || name[0] == 'v') && (name[1] == 'K' || name[1] == 'k')
        && name[2] == '_') {
        long v = strtol(name + 3, NULL, 16);
        if (v > 0 && v < 256)
            return (int)v;
    }
    return 0;
}

const char *mkb_input_name(int input)
{
    static char buf[4][8];
    static int k;
    size_t i;
    if ((input >= 'A' && input <= 'Z') || (input >= '0' && input <= '9')) {
        k = (k + 1) & 3;
        buf[k][0] = (char)input;
        buf[k][1] = 0;
        return buf[k];
    }
    for (i = 0; i < sizeof s_input_names / sizeof s_input_names[0]; i++)
        if (s_input_names[i].code == input)
            return s_input_names[i].name;
    if (input > 0 && input < 256) {
        k = (k + 1) & 3;
        snprintf(buf[k], sizeof buf[k], "VK_%02X", input);
        return buf[k];
    }
    return NULL;
}

int mkb_control_from_name(const char *name)
{
    int i;
    if (!name)
        return -1;
    for (i = 0; i < MKB_C_COUNT; i++)
        if (!_stricmp(name, s_control_names[i]))
            return i;
    return -1;
}

const char *mkb_control_name(int control)
{
    return control >= 0 && control < MKB_C_COUNT ? s_control_names[control] : NULL;
}

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.22, 1.0, 40, 0, 0 };

/* The default table: a generic first-person layout. A title's host
 * replaces it with its own (mkb_set_binds) or an ini does. */
static const MkbBind s_default_binds[] = {
    { 'W', MKB_C_LUP }, { 'S', MKB_C_LDOWN }, { 'A', MKB_C_LLEFT }, { 'D', MKB_C_LRIGHT },
    { MKB_IN_MOUSE1, MKB_C_RT }, { MKB_IN_MOUSE2, MKB_C_LT },
    { VK_SPACE, MKB_C_A }, { 'R', MKB_C_X }, { 'E', MKB_C_B },
    { 'Q', MKB_C_WHITE }, { 'F', MKB_C_BLACK },
    { MKB_IN_MOUSE4, MKB_C_WHITE }, { MKB_IN_MOUSE5, MKB_C_BLACK },
    { MKB_IN_MOUSE3, MKB_C_Y }, { MKB_IN_WHEELUP, MKB_C_Y }, { MKB_IN_WHEELDOWN, MKB_C_Y },
    { VK_RETURN, MKB_C_START }, { VK_ESCAPE, MKB_C_START }, { VK_TAB, MKB_C_BACK },
    { VK_LSHIFT, MKB_C_LS }, { VK_LCONTROL, MKB_C_RS },
    { VK_UP, MKB_C_DUP }, { VK_DOWN, MKB_C_DDOWN }, { VK_LEFT, MKB_C_DLEFT },
    { VK_RIGHT, MKB_C_DRIGHT },
};

static MkbBind s_binds[MKB_MAX_BINDS];
static volatile LONG s_nbinds = -1;     /* -1: defaults not loaded yet */
static SRWLOCK s_bind_lock = SRWLOCK_INIT;
static int s_ini_binds;                 /* bind lines seen in the ini */

void mkb_set_aim(double sens, int invert_y, double deadzone)
{
    if (sens > 0.0 && sens < 100.0) s_cfg.sens = sens;
    s_cfg.invert_y = invert_y != 0;
    if (deadzone >= 0.0 && deadzone < 0.9) s_cfg.dz = deadzone;
}

void mkb_set_binds(const MkbBind *binds, int n)
{
    if (n < 0) n = 0;
    if (n > MKB_MAX_BINDS) n = MKB_MAX_BINDS;
    AcquireSRWLockExclusive(&s_bind_lock);
    if (n)
        memcpy(s_binds, binds, (size_t)n * sizeof *binds);
    s_nbinds = n;
    ReleaseSRWLockExclusive(&s_bind_lock);
}

static void binds_default_if_unset(void)
{
    if (s_nbinds < 0)
        mkb_set_binds(s_default_binds,
                      (int)(sizeof s_default_binds / sizeof s_default_binds[0]));
}

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!_strnicmp(key, "bind.", 5)) {
        int in = mkb_input_from_name(key + 5), c = mkb_control_from_name(val);
        if (!in || c < 0)
            return 0;
        AcquireSRWLockExclusive(&s_bind_lock);
        if (!s_ini_binds)
            s_nbinds = 0;              /* the ini's table replaces the default */
        s_ini_binds = 1;
        if (s_nbinds < MKB_MAX_BINDS) {
            s_binds[s_nbinds].input = (uint16_t)in;
            s_binds[s_nbinds].control = (uint8_t)c;
            s_nbinds++;
        }
        ReleaseSRWLockExclusive(&s_bind_lock);
        return 1;
    }
    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    binds_default_if_unset();
    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            path[0] = 0;
        else
            snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = path[0] ? fopen(path, "r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
        fclose(f);
    }
    /* The environment wins over the file. */
    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel_up, s_wheel_down;   /* steps since last poll  */
static volatile LONG s_mouse_btn;    /* held: 1 L, 2 R, 4 M, 8 X1, 16 X2 */
static volatile LONG s_mouse_click;  /* pressed since the last poll      */
static volatile unsigned char s_vk_down[256];
static volatile unsigned char s_vk_tap[256];     /* pressed since last poll */

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static void mouse_button(USHORT flags, USHORT down, USHORT up, LONG bit)
{
    if (flags & down) {
        InterlockedOr(&s_mouse_btn, bit);
        InterlockedOr(&s_mouse_click, bit);
    }
    if (flags & up)
        InterlockedAnd(&s_mouse_btn, ~bit);
}

static void key_set(UINT vk, int down)
{
    if (!vk || vk > 255)
        return;
    if (down && !s_vk_down[vk])
        s_vk_tap[vk] = 1;
    s_vk_down[vk] = (unsigned char)(down ? 1 : 0);
}

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;
        USHORT f = m->usButtonFlags;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
        }
        mouse_button(f, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1);
        mouse_button(f, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2);
        mouse_button(f, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 4);
        mouse_button(f, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 8);
        mouse_button(f, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 16);
        if (f & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            if (d > 0) InterlockedIncrement(&s_wheel_up);
            else if (d < 0) InterlockedIncrement(&s_wheel_down);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;
        int down = !(k->Flags & RI_KEY_BREAK);

        if (vk == MKB_TOGGLE_VK) {
            if (down && !s_vk_down[vk]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        /* Raw input reports the generic Shift/Ctrl/Alt; the bindings name
         * the sides, as games do. */
        if (vk == VK_SHIFT)
            vk = k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        else if (vk == VK_CONTROL)
            vk = (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            vk = (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        key_set(vk, down);
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* A message-only window: it exists to own the WM_INPUT delivery. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", 0,
                           0, 0, 0, 0, HWND_MESSAGE, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld; %ld bindings)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full, (long)s_nbinds);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Whether the pointer should be hidden and confined now. A host with its
 * own window reads this on its UI thread (the cursor belongs to that
 * thread's input state); RECOMP_MKB_HOST_CURSOR=1 says it does. */
int mkb_capture_wanted(void)
{
    return mkb_enabled() && s_thread_up && s_active && !xbox_InputBlocked()
        && game_is_foreground();
}

/* Keyboard & mouse on or off from a settings screen (as F9 does). */
void mkb_set_active(int on)
{
    InterlockedExchange(&s_active, on ? 1 : 0);
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;
    static int host = -1;

    if (host < 0)
        host = getenv("RECOMP_MKB_HOST_CURSOR") != NULL;
    if (host)
        return;
    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* Per-axis dead zone compensation: each axis's [0..1] comes out as
 * [dz..1]. The title applies its own dead zone to each stick axis, so a
 * radial floor (the whole vector lifted to dz) left the smaller component of
 * a diagonal movement under the title's threshold: aim moved only straight
 * up, down, left or right. Per axis, every component that is there clears
 * it, and the response above it stays linear. */
static SHORT mkb_axis(double a)
{
    double m = fabs(a), out;
    long v;
    if (m < 1e-6)
        return 0;
    if (m > 1.0) m = 1.0;
    out = s_cfg.dz + (1.0 - s_cfg.dz) * pow(m, s_cfg.acc);
    if (out > 1.0) out = 1.0;
    v = (long)(out * 32767.0);
    return (SHORT)(a < 0 ? -v : v);
}

static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    *out_x = mkb_axis(ax);
    *out_y = mkb_axis(ay);
}

/* Is an input held for this poll? Keys and buttons count when held now or
 * pressed and released since the last poll (a quick tap still lands);
 * a wheel step holds its control for one poll. */
static int input_held(int in, LONG btn, LONG wheel_up, LONG wheel_down)
{
    if (in > 0 && in < 256)
        return s_vk_down[in] || s_vk_tap[in];
    switch (in) {
    case MKB_IN_MOUSE1: return (btn & 1) != 0;
    case MKB_IN_MOUSE2: return (btn & 2) != 0;
    case MKB_IN_MOUSE3: return (btn & 4) != 0;
    case MKB_IN_MOUSE4: return (btn & 8) != 0;
    case MKB_IN_MOUSE5: return (btn & 16) != 0;
    case MKB_IN_WHEELUP: return wheel_up != 0;
    case MKB_IN_WHEELDOWN: return wheel_down != 0;
    }
    return 0;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, btn;
    int any = 0, i, dir[8];
    static DWORD packet;
    static LONG wheel_hold_up, wheel_hold_down;   /* release poll after a step */
    static ULONGLONG last_poll_ms;      /* microseconds, QPC (GetTickCount64 moves in 16 ms steps) */
    static SHORT last_aim_x, last_aim_y;
    ULONGLONG now_ms;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up || xbox_InputBlocked())
        return 0;

    if (!s_active) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        InterlockedExchange(&s_wheel_up, 0);
        InterlockedExchange(&s_wheel_down, 0);
        InterlockedExchange(&s_mouse_click, 0);
        memset((void *)s_vk_tap, 0, sizeof s_vk_tap);
        return 0;
    }
    mkb_manage_cursor(1);
    binds_default_if_unset();

    /* A title that reads the pad twice in one frame would see the mouse
     * move in the first read and stop in the second; a read within 4 ms
     * of the last one repeats its aim and leaves the deltas to build up. */
    {
        static LARGE_INTEGER f;
        LARGE_INTEGER c;
        if (!f.QuadPart)
            QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        now_ms = (ULONGLONG)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
    }
    if (now_ms - last_poll_ms < 4000) {
        dx = dy = 0;
        aim_x = last_aim_x;
        aim_y = last_aim_y;
    } else {
        dx = InterlockedExchange(&s_dx, 0);
        dy = InterlockedExchange(&s_dy, 0);
        /* The deflection is the movement per poll scaled to a 60 Hz poll,
         * so the same mouse travel turns the view as far whatever rate the
         * title reads the pad at. */
        {
            double dt = last_poll_ms ? (double)(now_ms - last_poll_ms) / 1e6 : 1.0 / 60.0;
            double k;
            if (dt < 0.004) dt = 0.004;
            if (dt > 0.1) dt = 0.1;
            k = (1.0 / 60.0) / dt;
            last_poll_ms = now_ms;
            /* ---- mouse: aim ---- */
            if (dx || dy) {
                /* Sensitivity scales the counts, so every setting can still
                 * reach a full throw (and so the title's top turn rate). */
                double ax =  (double)dx * k * s_cfg.sens / (double)s_cfg.full;
                double ay = -(double)dy * k * s_cfg.sens / (double)s_cfg.full;
                if (s_cfg.invert_y) ay = -ay;
                mkb_aim_vector(ax, ay, &aim_x, &aim_y);
            }
        }
        last_aim_x = aim_x;
        last_aim_y = aim_y;
    }
    btn = s_mouse_btn | InterlockedExchange(&s_mouse_click, 0);

    /* A wheel step is a press for one poll and a release for the next, so
     * two steps in a row are two presses to a title looking for edges. */
    {
        LONG up = 0, down = 0;
        if (wheel_hold_up) wheel_hold_up = 0;
        else if (InterlockedExchange(&s_wheel_up, 0)) up = wheel_hold_up = 1;
        if (wheel_hold_down) wheel_hold_down = 0;
        else if (InterlockedExchange(&s_wheel_down, 0)) down = wheel_hold_down = 1;

        memset(analog, 0, sizeof analog);
        memset(dir, 0, sizeof dir);
        AcquireSRWLockShared(&s_bind_lock);
        for (i = 0; i < s_nbinds; i++) {
            int c = s_binds[i].control;
            if (!input_held(s_binds[i].input, btn, up, down))
                continue;
            if (c <= MKB_C_RT)
                analog[c] = 255;
            else if (c <= MKB_C_RS)
                b |= (WORD)(1u << (c - MKB_C_DUP));
            else if (c < MKB_C_COUNT)
                dir[c - MKB_C_LUP] = 1;
        }
        ReleaseSRWLockShared(&s_bind_lock);
        memset((void *)s_vk_tap, 0, sizeof s_vk_tap);
    }

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    {
        /* Keys on a stick: full throw, diagonals on the unit circle. */
        int lx = dir[3] - dir[2], ly = dir[0] - dir[1];
        int rx = dir[7] - dir[6], ry = dir[4] - dir[5];
        SHORT mx = 0, my = 0, kx = 0, ky = 0;
        if (lx || ly) {
            mx = (SHORT)(lx * (ly ? 23170 : 32767));
            my = (SHORT)(ly * (lx ? 23170 : 32767));
        }
        if (rx || ry) {
            kx = (SHORT)(rx * (ry ? 23170 : 32767));
            ky = (SHORT)(ry * (rx ? 23170 : 32767));
        }
        if (kx || ky) {             /* look keys win over the mouse */
            aim_x = kx;
            aim_y = ky;
        }
        if (aim_x || aim_y) {
            if (s_cfg.swap_sticks) {
                pState->Gamepad.sThumbLX = aim_x;
                pState->Gamepad.sThumbLY = aim_y;
            } else {
                pState->Gamepad.sThumbRX = aim_x;
                pState->Gamepad.sThumbRY = aim_y;
            }
            any = 1;
        }
        if (mx || my) {
            if (s_cfg.swap_sticks) {
                pState->Gamepad.sThumbRX = mx;
                pState->Gamepad.sThumbRY = my;
            } else {
                pState->Gamepad.sThumbLX = mx;
                pState->Gamepad.sThumbLY = my;
            }
            any = 1;
        }
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
