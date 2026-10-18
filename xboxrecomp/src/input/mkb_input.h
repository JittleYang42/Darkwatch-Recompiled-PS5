/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, keys and mouse buttons drive the
 * pad's buttons through a binding table -- regardless of which path the
 * title takes to read its pad, because the merge happens in
 * xbox_InputGetState, which every path goes through.
 *
 * Knobs (env vars, or the same keys in an ini file):
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 40)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.22),
 *                         lifted per axis
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file: the keys above without the prefix
 *                         (sens=1.5), plus bindings: bind.SPACE=Y,
 *                         bind.MOUSE1=RT. Any bind line replaces the
 *                         whole default table.
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inputs a binding can name: 1..255 are Windows virtual-key codes (the
 * left/right Shift, Ctrl and Alt are told apart), the rest the mouse. */
enum {
    MKB_IN_MOUSE1 = 0x101,      /* left   */
    MKB_IN_MOUSE2,              /* right  */
    MKB_IN_MOUSE3,              /* middle */
    MKB_IN_MOUSE4,              /* back side button  */
    MKB_IN_MOUSE5,              /* front side button */
    MKB_IN_WHEELUP,
    MKB_IN_WHEELDOWN,
    MKB_IN_LAST
};

/* What an input drives on the virtual pad. */
enum {
    MKB_C_A, MKB_C_B, MKB_C_X, MKB_C_Y,             /* analog buttons, in */
    MKB_C_BLACK, MKB_C_WHITE, MKB_C_LT, MKB_C_RT,   /* XBOX_BUTTON_* order */
    MKB_C_DUP, MKB_C_DDOWN, MKB_C_DLEFT, MKB_C_DRIGHT,
    MKB_C_START, MKB_C_BACK, MKB_C_LS, MKB_C_RS,    /* digital, wButtons order */
    MKB_C_LUP, MKB_C_LDOWN, MKB_C_LLEFT, MKB_C_LRIGHT,   /* stick directions */
    MKB_C_RUP, MKB_C_RDOWN, MKB_C_RLEFT, MKB_C_RRIGHT,
    MKB_C_COUNT
};

typedef struct {
    uint16_t input;             /* VK code or MKB_IN_* */
    uint8_t  control;           /* MKB_C_* */
} MkbBind;

#define MKB_MAX_BINDS 96

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/* Merge mouse+keyboard into a pad state already filled from the host
 * controller (or zeroed). Returns 1 when anything was merged. */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* Whether the pointer should be hidden and confined right now (enabled,
 * captured -- F9 releases -- and the game in the foreground). Hosts that own
 * their window poll this on the UI thread and set RECOMP_MKB_HOST_CURSOR=1. */
int mkb_capture_wanted(void);

/* On or off at run time (a settings screen; F9 toggles the same state). */
void mkb_set_active(int on);

/* Aim: sensitivity multiplier, inverted Y, and the per-axis dead zone the
 * title is assumed to have (0..0.9); any thread, at any time. */
void mkb_set_aim(double sens, int invert_y, double deadzone);

/* Replace the binding table (a host's settings screen). n <= MKB_MAX_BINDS.
 * Takes effect at the next poll. */
void mkb_set_binds(const MkbBind *binds, int n);

/* Names, as the ini spells them: "SPACE", "W", "MOUSE1", "WHEELUP"; and
 * "A", "RT", "LUP". Unknown: 0 / -1 / NULL. */
int         mkb_input_from_name(const char *name);
const char *mkb_input_name(int input);
int         mkb_control_from_name(const char *name);
const char *mkb_control_name(int control);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
