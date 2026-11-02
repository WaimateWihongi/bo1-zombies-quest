/* The user32/gdi32/shell32 calls the engine makes, and the Quest's versions of what
 * win32/win_wndproc.cpp and win32/win_syscon.cpp provide on Windows.
 *
 * The Quest has no desktop: there is one "window", always active and never minimized, the size
 * of the render target. No messages arrive through a queue (input comes from the VR layer), the
 * dedicated-server console is logcat, and the clipboard lives in memory. */
#include <windows.h>
#include <ShlObj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_internal.h"
#include "win32/win_wndproc.h"
#include "win32/win_syscon.h"

/* the render target the engine sees as its screen; the VR layer sets the real eye size later */
static const int SCREEN_WIDTH = 1920, SCREEN_HEIGHT = 1080;

WinVars_t g_wv;

static struct HWND__ g_window;
static struct HMONITOR__ g_monitor;
static struct HDC__ g_dc;
static struct HWND__ g_desktop;

/* ---- the window ------------------------------------------------------------------------------ */

ATOM RegisterClassExA(const WNDCLASSEXA *) { return 1; }
HICON LoadIconA(HINSTANCE, LPCSTR) { return NULL; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return NULL; }
HBRUSH CreateSolidBrush(COLORREF) { return NULL; }

HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HANDLE, HINSTANCE, LPVOID)
{
    g_wv.activeApp = 1;
    g_wv.isMinimized = 0;
    return &g_window;
}

BOOL DestroyWindow(HWND) { return TRUE; }
BOOL IsWindow(HWND window) { return window == &g_window; }
BOOL IsIconic(HWND) { return FALSE; }
BOOL ShowWindow(HWND, int) { return TRUE; }
HWND SetFocus(HWND window) { return window; }
HWND GetForegroundWindow(void) { return &g_window; }
HWND GetActiveWindow(void) { return &g_window; }
HWND GetDesktopWindow(void) { return &g_desktop; }
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
LONG GetWindowLongA(HWND, int) { return WS_POPUP | WS_VISIBLE; }
LONG SetWindowLongA(HWND, int, LONG value) { return value; }

int GetWindowTextA(HWND, LPSTR text, int size)
{
    if (size > 0) text[0] = 0;
    return 0;
}

static void screen_rect(LPRECT rect)
{
    rect->left = rect->top = 0;
    rect->right = SCREEN_WIDTH;
    rect->bottom = SCREEN_HEIGHT;
}

BOOL GetWindowRect(HWND, LPRECT rect)
{
    screen_rect(rect);
    return TRUE;
}

BOOL GetClientRect(HWND, LPRECT rect)
{
    screen_rect(rect);
    return TRUE;
}

BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }
BOOL ScreenToClient(HWND, LPPOINT) { return TRUE; }
BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }

BOOL EqualRect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

BOOL IsProcessDPIAware(void) { return TRUE; }

BOOL EnumThreadWindows(DWORD, WNDENUMPROC callback, LPARAM param)
{
    callback(&g_window, param);
    return TRUE;
}

/* ---- messages: none arrive ------------------------------------------------------------------- */

BOOL PeekMessageA(LPMSG, HWND, UINT, UINT, UINT) { return FALSE; }

BOOL GetMessageA(LPMSG message, HWND, UINT, UINT)
{
    /* only Sys_Quit's drain loop calls this: report WM_QUIT */
    memset(message, 0, sizeof *message);
    return FALSE;
}

BOOL TranslateMessage(const MSG *) { return FALSE; }
LRESULT DispatchMessageA(const MSG *) { return 0; }
BOOL PostMessageA(HWND, UINT, WPARAM, LPARAM) { return TRUE; }

int MainWndProc(HWND__ *, UINT, signed int, LPARAM) { return 0; }

/* ---- display and monitors -------------------------------------------------------------------- */

int GetSystemMetrics(int index)
{
    switch (index) {
    case SM_CXSCREEN: return SCREEN_WIDTH;
    case SM_CYSCREEN: return SCREEN_HEIGHT;
    case 80: return 1; /* SM_CMONITORS */
    default: return 0; /* SM_REMOTESESSION (0x1000) and the rest */
    }
}

static void fill_monitor(MONITORINFO *info)
{
    screen_rect(&info->rcMonitor);
    screen_rect(&info->rcWork);
    info->dwFlags = 1; /* MONITORINFOF_PRIMARY */
}

BOOL GetMonitorInfoA(HMONITOR, MONITORINFO *info)
{
    fill_monitor(info);
    if (info->cbSize >= sizeof(MONITORINFOEXA)) strcpy(((MONITORINFOEXA *)info)->szDevice, "\\\\.\\DISPLAY1");
    return TRUE;
}

HMONITOR MonitorFromPoint(POINT, DWORD) { return &g_monitor; }
HMONITOR MonitorFromWindow(HWND, DWORD) { return &g_monitor; }

BOOL EnumDisplayMonitors(HDC, const RECT *, MONITORENUMPROC callback, LPARAM param)
{
    RECT r;
    screen_rect(&r);
    callback(&g_monitor, &g_dc, &r, param);
    return TRUE;
}

BOOL EnumDisplaySettingsA(LPCSTR, DWORD mode, DEVMODEA *dev_mode)
{
    if (mode != ENUM_CURRENT_SETTINGS && mode != 0) return FALSE;
    dev_mode->dmPelsWidth = SCREEN_WIDTH;
    dev_mode->dmPelsHeight = SCREEN_HEIGHT;
    dev_mode->dmBitsPerPel = 32;
    dev_mode->dmDisplayFrequency = 72;
    return TRUE;
}

LONG ChangeDisplaySettingsA(void *, DWORD) { return 0; }
HDC GetDC(HWND) { return &g_dc; }
int ReleaseDC(HWND, HDC) { return 1; }
BOOL SetDeviceGammaRamp(HDC, LPVOID) { return TRUE; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT) { return TRUE; }

/* ---- cursor ---------------------------------------------------------------------------------- */

static POINT g_cursor;
int ShowCursor(BOOL show) { return show ? 0 : -1; }

BOOL SetCursorPos(int x, int y)
{
    g_cursor.x = x;
    g_cursor.y = y;
    return TRUE;
}

BOOL GetCursorPos(LPPOINT point)
{
    *point = g_cursor;
    return TRUE;
}

/* ---- clipboard (process-local) --------------------------------------------------------------- */

static HANDLE g_clipboard;
BOOL OpenClipboard(HWND) { return TRUE; }
BOOL CloseClipboard(void) { return TRUE; }

BOOL EmptyClipboard(void)
{
    if (g_clipboard) GlobalFree(g_clipboard);
    g_clipboard = NULL;
    return TRUE;
}

HANDLE SetClipboardData(UINT, HANDLE data)
{
    EmptyClipboard();
    g_clipboard = data;
    return data;
}

HANDLE GetClipboardData(UINT) { return g_clipboard; }

/* ---- shell ----------------------------------------------------------------------------------- */

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT)
{
    bo1_log("[message box] %s: %s", caption ? caption : "", text ? text : "");
    return IDOK;
}

HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR file, LPCSTR, LPCSTR, int)
{
    bo1_log("ShellExecute %s: not on the Quest", file ? file : "");
    return NULL;
}

/* "My Documents", AppData and the like: the data folder, where profiles and configs go */
HRESULT SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR path)
{
    snprintf(path, MAX_PATH, "%s", bo1_data_root());
    return S_OK;
}

/* ---- win_wndproc.cpp's other exports --------------------------------------------------------- */

void Sys_UpdateHotkeyBlock() {}

/* ---- win_syscon.cpp: the console is logcat --------------------------------------------------- */

void Sys_ShowConsole() {}
void Sys_DestroyConsole() {}
/* Win_GetEvent calls this every frame on the main thread, holding the event queue: where
   Android's input joins the engine's events (quest_input.cpp) */
void quest_input_pump();
char *Sys_ConsoleInput()
{
    quest_input_pump();
    return NULL;
}

void Sys_SetErrorText(const char *text) { bo1_log("ERROR: %s", text); }

/* console lines carry ^n colour codes; logcat wants them gone and a line at a time */
void Conbuf_AppendText(char *text)
{
    static char line[4096];
    static size_t used;
    for (const char *p = text; *p; p++) {
        if (*p == '^' && p[1] >= '0' && p[1] <= '9') {
            p++;
            continue;
        }
        if (*p == '\n' || used == sizeof line - 1) {
            line[used] = 0;
            bo1_log("%s", line);
            used = 0;
            if (*p == '\n') continue;
        }
        if (*p != '\r') line[used++] = *p;
    }
}

void Conbuf_AppendTextInMainThread(char *text) { Conbuf_AppendText(text); }
