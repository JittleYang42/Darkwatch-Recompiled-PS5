/*
 * dw_tables.c -- the PC port's action and controller tables and the default
 * settings. No platform calls: the launcher preview builds this on Linux.
 */
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "dw_settings.h"
#include "dw_vk.h"

/* Darkwatch's Xbox layout (from the manual): A interact, B reload, X melee,
 * Y jump, White activate power, Black cycle powers, left trigger throw,
 * right trigger shoot, left stick click crouch, right stick click sight
 * mode, D-pad up/down cycle thrown/projectile weapons, left/right lean,
 * Back score menu, Start in-game menu. */
#define K(c) ((uint16_t)(c))
const DwAction dw_actions[] = {
    { "move_forward",  "Move forward",              MKB_C_LUP,    { K('W'), 0 } },
    { "move_back",     "Move back",                 MKB_C_LDOWN,  { K('S'), 0 } },
    { "strafe_left",   "Strafe left",               MKB_C_LLEFT,  { K('A'), 0 } },
    { "strafe_right",  "Strafe right",              MKB_C_LRIGHT, { K('D'), 0 } },
    { "shoot",         "Shoot",                     MKB_C_RT,     { MKB_IN_MOUSE1, 0 } },
    { "sight",         "Sight mode / zoom",         MKB_C_RS,     { MKB_IN_MOUSE2, 0 } },
    { "jump",          "Jump (twice: vampire jump)", MKB_C_Y,     { K(VK_SPACE), 0 } },
    { "interact",      "Interact / menu confirm",   MKB_C_A,      { K('E'), K(VK_RETURN) } },
    { "reload",        "Reload / menu back",        MKB_C_B,      { K('R'), K(VK_BACK) } },
    { "melee",         "Melee",                     MKB_C_X,      { K('F'), K('V') } },
    { "throw",         "Throw",                     MKB_C_LT,     { K('G'), MKB_IN_MOUSE4 } },
    { "power",         "Activate blood power",      MKB_C_WHITE,  { K('Q'), 0 } },
    { "cycle_power",   "Cycle blood powers",        MKB_C_BLACK,  { K('T'), 0 } },
    { "crouch",        "Crouch",                    MKB_C_LS,     { K(VK_LCONTROL), K('C') } },
    { "next_gun",      "Cycle guns",                MKB_C_DDOWN,  { MKB_IN_WHEELDOWN, K('1') } },
    { "next_thrown",   "Cycle thrown weapons",      MKB_C_DUP,    { MKB_IN_WHEELUP, K('2') } },
    { "lean_left",     "Lean left",                 MKB_C_DLEFT,  { K('Z'), 0 } },
    { "lean_right",    "Lean right",                MKB_C_DRIGHT, { K('X'), 0 } },
    { "pause",         "Pause menu (Start)",        MKB_C_START,  { K(VK_ESCAPE), 0 } },
    { "score",         "Score menu (Back)",         MKB_C_BACK,   { K(VK_TAB), 0 } },
    { "menu_up",       "Menu up (D-pad)",           MKB_C_DUP,    { K(VK_UP), 0 } },
    { "menu_down",     "Menu down (D-pad)",         MKB_C_DDOWN,  { K(VK_DOWN), 0 } },
    { "menu_left",     "Menu left (D-pad)",         MKB_C_DLEFT,  { K(VK_LEFT), 0 } },
    { "menu_right",    "Menu right (D-pad)",        MKB_C_DRIGHT, { K(VK_RIGHT), 0 } },
    { "look_up",       "Look up (keys)",            MKB_C_RUP,    { 0, 0 } },
    { "look_down",     "Look down (keys)",          MKB_C_RDOWN,  { 0, 0 } },
    { "look_left",     "Look left (keys)",          MKB_C_RLEFT,  { 0, 0 } },
    { "look_right",    "Look right (keys)",         MKB_C_RRIGHT, { 0, 0 } },
};
const int dw_action_count = (int)(sizeof dw_actions / sizeof dw_actions[0]);

const DwPadRow dw_pad_rows[16] = {
    { MKB_C_RT,     "Shoot" },
    { MKB_C_RS,     "Sight mode / zoom" },
    { MKB_C_Y,      "Jump" },
    { MKB_C_A,      "Interact / confirm" },
    { MKB_C_B,      "Reload / back" },
    { MKB_C_X,      "Melee" },
    { MKB_C_LT,     "Throw" },
    { MKB_C_WHITE,  "Activate blood power" },
    { MKB_C_BLACK,  "Cycle blood powers" },
    { MKB_C_LS,     "Crouch" },
    { MKB_C_DDOWN,  "Cycle guns" },
    { MKB_C_DUP,    "Cycle thrown weapons" },
    { MKB_C_DLEFT,  "Lean left" },
    { MKB_C_DRIGHT, "Lean right" },
    { MKB_C_START,  "Pause menu" },
    { MKB_C_BACK,   "Score menu" },
};

const char *dw_pad_button_name(int phys)
{
    static const char *const n[16] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
        "Start", "Back", "L-Stick Click", "R-Stick Click",
    };
    return phys >= 0 && phys < 16 ? n[phys] : "?";
}

void dw_settings_defaults(DwSettings *s)
{
    int i;
    memset(s, 0, sizeof *s);
    s->fullscreen = 1;
    s->res_scale = 0;
    s->interp = 1;
    s->vsync = 1;
    s->fps_cap = 0;
    s->aspect = 0;
    s->gpu = -1;
    s->mkb = 1;
    s->mouse_sens = 100;
    s->rumble = 1;
    s->volume = 100;
    s->mute_unfocused = 0;
    s->kick_lag = 1;
    s->xf_threads = -1;
    for (i = 0; i < dw_action_count && i < 64; i++) {
        s->keys[i][0] = dw_actions[i].def[0];
        s->keys[i][1] = dw_actions[i].def[1];
    }
    for (i = 0; i < 16; i++)
        s->pad_src[i] = (uint8_t)i;
}

