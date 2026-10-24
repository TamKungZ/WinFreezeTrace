#include "ui.h"
#include "startup.h"

#include <shellapi.h>

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NIF_INFO
#define NIF_INFO 0x00000010
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (NIN_SELECT | 0x1)
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif
#ifndef NIIF_NOSOUND
#define NIIF_NOSOUND 0x00000010
#endif

#define WM_WFT_TRAY (WM_APP + 1)     /* tray icon callback                          */
#define WM_WFT_BUSY (WM_APP + 2)     /* refresh icon after ui_set_capturing()       */
#define WM_WFT_DONE (WM_APP + 3)     /* wParam: malloc'ed folder (or NULL), lParam: ok */

enum { ID_TOGGLE = 1, ID_STARTUP, ID_FOLDER, ID_EXIT };
enum { ICON_ON, ICON_OFF, ICON_BUSY, ICON_COUNT };

static volatile LONG g_manual = 0, g_resume = 0, g_hotkey_ok = 0, g_paused = 0, g_busy = 0;
static HANDLE g_stop_event = NULL, g_wake_event = NULL, g_thread = NULL, g_ready = NULL;
static HWND   g_hwnd = NULL;
static HINSTANCE g_inst = NULL;
static UiOptions g_opt;
static WCHAR  g_reports[WFT_PATH_CCH], g_run_args[2048];

/* Everything below is touched by the UI thread only. */
static HICON  g_icons[ICON_COUNT];
static bool   g_tray_added = false, g_tray_v4 = false, g_exiting = false;
static WCHAR  g_last_report[WFT_PATH_CCH];
static DWORD  g_last_toggle_tick = 0;
static UINT   g_msg_taskbar = 0, g_msg_show = 0, g_msg_exit = 0;

#define MSG_SHOW_NAME L"WinFreezeTrace.ShowRunning.v1"
#define MSG_EXIT_NAME L"WinFreezeTrace.Exit.v1"

bool ui_take_manual(void)       { return InterlockedExchange(&g_manual, 0) != 0; }
bool ui_take_resume(void)       { return InterlockedExchange(&g_resume, 0) != 0; }
bool ui_hotkey_registered(void) { return g_hotkey_ok != 0; }
bool ui_paused(void)            { return g_paused != 0; }

/* ---- generated icons: green check = recording, grey bars = paused, orange dot = writing a report ---- */

static double seg_dist2(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    double t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return dx * dx + dy * dy;
}

/* Colour of the icon at (x,y) in the unit square; false = transparent. */
static bool shade(int state, double x, double y, double rgb[3]) {
    static const double fill[ICON_COUNT][3] = { { 46, 160, 80 }, { 122, 128, 138 }, { 232, 137, 12 } };
    double dx = x - 0.5, dy = y - 0.5, r2 = dx * dx + dy * dy;
    if (r2 > 0.47 * 0.47) return false;
    bool symbol;
    if (state == ICON_ON)       symbol = seg_dist2(x, y, 0.26, 0.53, 0.43, 0.69) < 0.065 * 0.065 || seg_dist2(x, y, 0.43, 0.69, 0.75, 0.32) < 0.065 * 0.065;
    else if (state == ICON_OFF) symbol = ((x >= 0.30 && x <= 0.44) || (x >= 0.56 && x <= 0.70)) && y >= 0.28 && y <= 0.72;
    else                        symbol = r2 < 0.17 * 0.17;
    double k = symbol ? 0.0 : (r2 > 0.41 * 0.41 ? 0.62 : 1.0);       /* darker rim */
    for (int i = 0; i < 3; ++i) rgb[i] = symbol ? 255.0 : fill[state][i] * k;
    return true;
}

static HICON make_icon(int state) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 16) sz = 16;
    if (sz > 64) sz = 64;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    size_t mask_bytes = (size_t)((sz + 15) / 16) * 2u * (size_t)sz;
    BYTE *zero = (BYTE *)calloc(mask_bytes, 1);
    HBITMAP mask = zero ? CreateBitmap(sz, sz, 1, 1, zero) : NULL;
    free(zero);
    HICON icon = NULL;
    if (color && bits && mask) {
        DWORD *px = (DWORD *)bits;
        for (int y = 0; y < sz; ++y) for (int x = 0; x < sz; ++x) {
            double sr = 0, sg = 0, sb = 0; int hit = 0;
            for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {      /* 4x4 supersampling = smooth edges */
                double c[3];
                if (shade(state, (x + (i + 0.5) / 4.0) / sz, (y + (j + 0.5) / 4.0) / sz, c)) { sr += c[0]; sg += c[1]; sb += c[2]; ++hit; }
            }
            DWORD a = (DWORD)(hit * 255 / 16);
            /* premultiplied ARGB */
            DWORD r = hit ? (DWORD)(sr / 16.0 + 0.5) : 0, g = hit ? (DWORD)(sg / 16.0 + 0.5) : 0, b = hit ? (DWORD)(sb / 16.0 + 0.5) : 0;
            px[y * sz + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        ICONINFO ii; memset(&ii, 0, sizeof ii);
        ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = color;
        icon = CreateIconIndirect(&ii);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

/* ---- tray icon ---- */

static int current_icon(void) { return g_busy ? ICON_BUSY : (g_paused ? ICON_OFF : ICON_ON); }

static void tray_fill(NOTIFYICONDATAW *n, UINT flags) {
    memset(n, 0, sizeof *n);
    n->cbSize = sizeof *n; n->hWnd = g_hwnd; n->uID = 1; n->uFlags = flags;
}

static void tray_tip(WCHAR *tip, size_t cch) {
    if (g_busy)        wft_wsnprintf(tip, cch, L"WinFreezeTrace %hs - writing a report...", WFT_VERSION);
    else if (g_paused) wft_wsnprintf(tip, cch, L"WinFreezeTrace %hs - monitoring OFF (click to turn on)", WFT_VERSION);
    else               wft_wsnprintf(tip, cch, L"WinFreezeTrace %hs - monitoring ON (click to turn off)", WFT_VERSION);
}

static void tray_modify(void) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW n; tray_fill(&n, NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    n.hIcon = g_icons[current_icon()];
    tray_tip(n.szTip, sizeof n.szTip / sizeof n.szTip[0]);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static bool tray_add(void) {
    if (!g_opt.tray || g_exiting || !g_hwnd) return false;
    NOTIFYICONDATAW n; tray_fill(&n, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    n.uCallbackMessage = WM_WFT_TRAY;
    n.hIcon = g_icons[current_icon()];
    tray_tip(n.szTip, sizeof n.szTip / sizeof n.szTip[0]);
    if (!Shell_NotifyIconW(NIM_ADD, &n)) return false;
    g_tray_added = true;
    NOTIFYICONDATAW v; tray_fill(&v, 0);
    v.uVersion = NOTIFYICON_VERSION_4;
    g_tray_v4 = Shell_NotifyIconW(NIM_SETVERSION, &v) != FALSE;   /* modern keyboard/mouse semantics (Vista+) */
    return true;
}

static void tray_remove(void) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW n; tray_fill(&n, 0);
    Shell_NotifyIconW(NIM_DELETE, &n);
    g_tray_added = false;
}

static void balloon(const WCHAR *title, const WCHAR *text, bool important) {
    if (!g_tray_added || (!g_opt.notify && !important)) return;
    NOTIFYICONDATAW n; tray_fill(&n, NIF_INFO);
    wft_wsnprintf(n.szInfoTitle, sizeof n.szInfoTitle / sizeof n.szInfoTitle[0], L"%ls", title);
    wft_wsnprintf(n.szInfo, sizeof n.szInfo / sizeof n.szInfo[0], L"%ls", text);
    n.dwInfoFlags = important ? NIIF_WARNING : (NIIF_INFO | NIIF_NOSOUND);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void open_folder(const WCHAR *path) {
    if (path && path[0]) ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void set_paused(bool paused) {
    InterlockedExchange(&g_paused, paused ? 1 : 0);
    log_msg("INFO", paused ? "monitoring turned OFF from the tray (sampling and triggers paused)" : "monitoring turned ON from the tray");
    tray_modify();
    if (g_wake_event) SetEvent(g_wake_event);
}

static void request_exit(const char *why) {
    log_msg("INFO", "exit requested (%s)", why);
    g_exiting = true;
    tray_remove();                         /* disappear at once, even if a capture is still finishing */
    if (g_stop_event) SetEvent(g_stop_event);
}

static void toggle_startup(void) {
    bool want = !startup_is_enabled();
    if (startup_set(want, g_run_args)) {
        log_msg("INFO", want ? "added to Windows Startup apps" : "removed from Windows Startup apps");
        balloon(L"WinFreezeTrace", want ? L"It will now start when you sign in to Windows." : L"It will no longer start with Windows.", false);
    } else {
        log_msg("ERROR", "could not %s the Startup apps entry (error %lu)", want ? "create" : "remove", (unsigned long)GetLastError());
        balloon(L"WinFreezeTrace", L"Could not change the Windows Startup setting.", true);
    }
}

static void show_menu(int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    WCHAR title[160];
    wft_wsnprintf(title, 160, L"WinFreezeTrace %hs - monitoring %ls", WFT_VERSION, g_paused ? L"OFF" : L"ON");
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, title);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TOGGLE, g_paused ? L"Turn monitoring &ON" : L"Turn monitoring &OFF");
    AppendMenuW(m, MF_STRING | (startup_is_enabled() ? MF_CHECKED : MF_UNCHECKED), ID_STARTUP, L"&Start with Windows");
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open &reports folder");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"E&xit");
    SetMenuDefaultItem(m, ID_TOGGLE, FALSE);

    SetForegroundWindow(g_hwnd);           /* required, otherwise the menu does not close when the user clicks elsewhere */
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    int cmd = (int)TrackPopupMenuEx(m, flags, x, y, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case ID_TOGGLE:  set_paused(!g_paused); break;
        case ID_STARTUP: toggle_startup(); break;
        case ID_FOLDER:  open_folder(g_reports); break;
        case ID_EXIT:    request_exit("tray menu"); break;
    }
}

static void tray_select(void) {
    DWORD now = GetTickCount();
    if (g_last_toggle_tick && now - g_last_toggle_tick < GetDoubleClickTime()) return;   /* a double-click must toggle once */
    g_last_toggle_tick = now;
    set_paused(!g_paused);
}

static void on_tray_event(WPARAM w, LPARAM l) {
    UINT ev = g_tray_v4 ? (UINT)LOWORD(l) : (UINT)l;
    if (g_tray_v4) {
        switch (ev) {
            case NIN_SELECT: case NIN_KEYSELECT: tray_select(); break;
            case WM_CONTEXTMENU: show_menu((int)(short)LOWORD(w), (int)(short)HIWORD(w)); break;
            case NIN_BALLOONUSERCLICK: open_folder(g_last_report[0] ? g_last_report : g_reports); break;
        }
    } else {                                                        /* legacy callback semantics */
        POINT pt; GetCursorPos(&pt);
        if (ev == WM_LBUTTONUP) tray_select();
        else if (ev == WM_RBUTTONUP) show_menu(pt.x, pt.y);
        else if (ev == NIN_BALLOONUSERCLICK) open_folder(g_last_report[0] ? g_last_report : g_reports);
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_msg_taskbar && m == g_msg_taskbar) {                      /* Explorer restarted: its tray was wiped */
        g_tray_added = false;
        if (tray_add()) log_msg("INFO", "tray icon restored after the taskbar was re-created");
        return 0;
    }
    if (g_msg_show && m == g_msg_show) {
        balloon(L"WinFreezeTrace is already running", g_paused ? L"Monitoring is OFF. Click the tray icon to turn it on." : L"Monitoring is ON. Use the tray icon to turn it off or exit.", false);
        return 0;
    }
    if (g_msg_exit && m == g_msg_exit) { request_exit("--exit from another instance"); return 0; }

    switch (m) {
        case WM_WFT_TRAY:
            on_tray_event(w, l);
            return 0;
        case WM_WFT_BUSY:
            tray_modify();
            return 0;
        case WM_WFT_DONE: {
            WCHAR *dir = (WCHAR *)w;
            if (dir) wft_wsnprintf(g_last_report, WFT_PATH_CCH, L"%ls", dir);
            if (l && dir) balloon(L"Freeze report saved", L"Click here to open the report folder. Start with summary.txt.", false);
            else if (!l)  balloon(L"WinFreezeTrace could not save a report", L"See winfreezetrace.log in the WinFreezeTrace folder for details.", true);
            free(dir);
            return 0;
        }
        case WM_HOTKEY:
            if (w == 1) {
                if (g_paused) balloon(L"Monitoring is OFF", L"Turn monitoring on from the tray icon to use the capture hotkey.", false);
                else { InterlockedExchange(&g_manual, 1); if (g_wake_event) SetEvent(g_wake_event); }
            }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) { log_msg("INFO", "system suspending"); InterlockedExchange(&g_resume, 1); }
            else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) { log_msg("INFO", "system resumed"); InterlockedExchange(&g_resume, 1); }
            return TRUE;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (w) { g_exiting = true; tray_remove(); if (g_stop_event) SetEvent(g_stop_event); }
            return 0;
        case WM_DESTROY:
            tray_remove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI ui_thread(LPVOID unused) {
    (void)unused;
    static const WCHAR cls[] = L"WinFreezeTrace.Hidden";
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wnd_proc; wc.hInstance = g_inst; wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    /* A plain (never shown) top-level window is required: message-only windows do not get power broadcasts or broadcasts from --exit. */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"WinFreezeTrace", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (g_hwnd && g_opt.hotkey) {
        if (RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12)) InterlockedExchange(&g_hotkey_ok, 1);
        else log_msg("WARN", "RegisterHotKey(Ctrl+Alt+F12) failed (error %lu): another program may own it. Manual capture is unavailable.", (unsigned long)GetLastError());
    }
    if (!g_hwnd) log_msg("WARN", "hidden window creation failed (error %lu): no hotkey, no tray icon, no power notifications", (unsigned long)GetLastError());
    if (g_hwnd && g_opt.tray) {
        for (int i = 0; i < ICON_COUNT; ++i) g_icons[i] = make_icon(i);
        if (!tray_add()) log_msg("WARN", "tray icon could not be added (is Explorer running?); it will be added when the taskbar appears");
    }
    SetEvent(g_ready);
    if (!g_hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_hotkey_ok) UnregisterHotKey(g_hwnd, 1);
    tray_remove();
    for (int i = 0; i < ICON_COUNT; ++i) if (g_icons[i]) { DestroyIcon(g_icons[i]); g_icons[i] = NULL; }
    return 0;
}

bool ui_start(HINSTANCE inst, HANDLE stop_event, HANDLE wake_event, const UiOptions *opt) {
    g_inst = inst; g_stop_event = stop_event; g_wake_event = wake_event;
    g_opt = *opt;
    wft_wsnprintf(g_reports, WFT_PATH_CCH, L"%ls", opt->reports_dir ? opt->reports_dir : L"");
    wft_wsnprintf(g_run_args, sizeof g_run_args / sizeof g_run_args[0], L"%ls", opt->run_args ? opt->run_args : L"");
    g_opt.reports_dir = g_reports; g_opt.run_args = g_run_args;
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    g_msg_show = RegisterWindowMessageW(MSG_SHOW_NAME);
    g_msg_exit = RegisterWindowMessageW(MSG_EXIT_NAME);
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ready) return false;
    g_thread = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);       /* default priority: it must react while the system is under load */
    if (!g_thread) return false;
    WaitForSingleObject(g_ready, 5000);
    return g_hwnd != NULL;
}

void ui_stop(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_ready) { CloseHandle(g_ready); g_ready = NULL; }
}

void ui_set_capturing(bool on) {
    InterlockedExchange(&g_busy, on ? 1 : 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_WFT_BUSY, 0, 0);
}

void ui_capture_done(const WCHAR *dir, bool ok) {
    WCHAR *copy = (dir && dir[0]) ? _wcsdup(dir) : NULL;
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_WFT_DONE, (WPARAM)copy, ok ? 1 : 0)) free(copy);
}

void ui_ping_running_instance(void) {
    UINT m = RegisterWindowMessageW(MSG_SHOW_NAME);
    if (m) PostMessageW(HWND_BROADCAST, m, 0, 0);
}

void ui_request_running_instance_exit(void) {
    UINT m = RegisterWindowMessageW(MSG_EXIT_NAME);
    if (m) PostMessageW(HWND_BROADCAST, m, 0, 0);
}
