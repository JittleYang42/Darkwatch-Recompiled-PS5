/* win_host.h -- what the Windows host's window offers the launcher. */
#ifndef DW_WIN_HOST_H
#define DW_WIN_HOST_H
#include <windows.h>

extern HWND g_dw_hwnd;
void dw_set_fullscreen(int on);
int  dw_is_fullscreen(void);
int  dw_display_height(void);       /* the monitor the window is on */
int  dw_display_refresh(void);      /* its refresh rate, Hz */

#endif
