#include "hostwin.h"
#include "control.h"

#define HOTKEY_ID 1

static volatile LONG g_resume = 0;
static HANDLE g_stop_event = NULL, g_thread = NULL, g_ready = NULL;
static HWND   g_hwnd = NULL;
static HINSTANCE g_inst = NULL;
static HostOptions g_opt;
static bool   g_hotkey_ok = false;
static UINT   g_msg_exit = 0;

#define MSG_EXIT_NAME L"WinFreezeTrace.Exit.v1"

bool host_take_resume(void) { return InterlockedExchange(&g_resume, 0) != 0; }

static void request_exit(const char *why) {
    log_msg("INFO", "exit requested (%s)", why);
    if (g_stop_event) SetEvent(g_stop_event);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_msg_exit && m == g_msg_exit) { request_exit("--exit from another instance"); return 0; }

    switch (m) {
        case WM_HOTKEY:
            if (w == HOTKEY_ID && !control_paused()) control_request_capture();     /* off = no hotkey, as documented */
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) { log_msg("INFO", "system suspending"); InterlockedExchange(&g_resume, 1); }
            else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) { log_msg("INFO", "system resumed"); InterlockedExchange(&g_resume, 1); }
            return TRUE;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (w && g_stop_event) SetEvent(g_stop_event);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI host_thread(LPVOID unused) {
    (void)unused;
    static const WCHAR cls[] = L"WinFreezeTrace.Hidden";
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wnd_proc; wc.hInstance = g_inst; wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    /* A plain (never shown) top-level window is required: message-only windows do not get power broadcasts or broadcasts from --exit. */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"WinFreezeTrace", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (g_hwnd && g_opt.hotkey) {
        if (RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12)) { g_hotkey_ok = true; control_set_hotkey_registered(true); }
        else log_msg("WARN", "RegisterHotKey(Ctrl+Alt+F12) failed (error %lu): another program may own it. Manual capture by hotkey is unavailable.", (unsigned long)GetLastError());
    }
    if (!g_hwnd) log_msg("WARN", "hidden window creation failed (error %lu): no hotkey and no power notifications", (unsigned long)GetLastError());
    SetEvent(g_ready);
    if (!g_hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_hotkey_ok) UnregisterHotKey(g_hwnd, HOTKEY_ID);
    return 0;
}

bool host_start(HINSTANCE inst, HANDLE stop_event, const HostOptions *opt) {
    g_inst = inst; g_stop_event = stop_event; g_opt = *opt;
    g_msg_exit = RegisterWindowMessageW(MSG_EXIT_NAME);
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ready) return false;
    g_thread = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);       /* default priority: it must react while the system is under load */
    if (!g_thread) return false;
    WaitForSingleObject(g_ready, 5000);
    return g_hwnd != NULL;
}

void host_stop(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_ready) { CloseHandle(g_ready); g_ready = NULL; }
}

void host_request_running_instance_exit(void) {
    UINT m = RegisterWindowMessageW(MSG_EXIT_NAME);
    if (m) PostMessageW(HWND_BROADCAST, m, 0, 0);
}
