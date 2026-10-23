#include "ui.h"

static volatile LONG g_manual = 0, g_resume = 0, g_hotkey_ok = 0;
static HANDLE g_stop_event = NULL, g_thread = NULL, g_ready = NULL;
static HWND   g_hwnd = NULL;
static HINSTANCE g_inst = NULL;
static bool   g_want_hotkey = true;

bool ui_take_manual(void)       { return InterlockedExchange(&g_manual, 0) != 0; }
bool ui_take_resume(void)       { return InterlockedExchange(&g_resume, 0) != 0; }
bool ui_hotkey_registered(void) { return g_hotkey_ok != 0; }

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HOTKEY:
            if (w == 1) InterlockedExchange(&g_manual, 1);
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

static DWORD WINAPI ui_thread(LPVOID unused) {
    (void)unused;
    static const WCHAR cls[] = L"WinFreezeTrace.Hidden";
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wnd_proc; wc.hInstance = g_inst; wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    /* A plain (never shown) top-level window is required: message-only windows do not get power broadcasts. */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"WinFreezeTrace", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (g_hwnd && g_want_hotkey) {
        if (RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12)) InterlockedExchange(&g_hotkey_ok, 1);
        else log_msg("WARN", "RegisterHotKey(Ctrl+Alt+F12) failed (error %lu): another program may own it. Manual capture is unavailable.", (unsigned long)GetLastError());
    }
    if (!g_hwnd) log_msg("WARN", "hidden window creation failed (error %lu): no hotkey, no power notifications", (unsigned long)GetLastError());
    SetEvent(g_ready);
    if (!g_hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_hotkey_ok) UnregisterHotKey(g_hwnd, 1);
    return 0;
}

bool ui_start(HINSTANCE inst, HANDLE stop_event, bool want_hotkey) {
    g_inst = inst; g_stop_event = stop_event; g_want_hotkey = want_hotkey;
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ready) return false;
    g_thread = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (!g_thread) return false;
    SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    WaitForSingleObject(g_ready, 5000);
    return g_hwnd != NULL;
}

void ui_stop(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_ready) { CloseHandle(g_ready); g_ready = NULL; }
}
