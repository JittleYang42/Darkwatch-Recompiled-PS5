/* launcher.h -- the start screen and settings, drawn into the game window
 * before the game boots (launcher.c; the drawing is ui_draw.c). */
#ifndef DW_LAUNCHER_H
#define DW_LAUNCHER_H
#include <windows.h>
#include <stdint.h>

enum { LAUNCHER_NONE, LAUNCHER_PLAY, LAUNCHER_QUIT, LAUNCHER_RESUME };

/* Take over the window; err (may be NULL) is shown as a notice. */
void launcher_open(HWND hwnd, const char *err);
/* Once per message-loop turn: input, animation, drawing. */
int  launcher_tick(void);
/* The settings over the running game (F1): drawn through the Vulkan
 * swapchain; backdrop is the game's last frame (640 x 480, may be NULL). */
void launcher_open_overlay(HWND hwnd, const uint32_t *backdrop);
void launcher_close(void);
/* The window procedure's hook while the launcher is up: returns 1 when it
 * handled the message (*result set). */
int  launcher_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result);

#endif
