#include "control.h"

#include <sddl.h>

#define PIPE_PREFIX L"\\\\.\\pipe\\WinFreezeTrace.Control.v1."
#define IO_TIMEOUT_MS 2000

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

static volatile LONG g_paused = 0, g_capturing = 0, g_manual = 0, g_hotkey_ok = 0;
static volatile LONG g_captures = 0, g_failures = 0, g_last_ok = 0;
static CRITICAL_SECTION g_lock;                  /* guards g_last_dir */
static bool   g_lock_ready = false;
static WCHAR  g_last_dir[WFT_PATH_CCH], g_reports[WFT_PATH_CCH];
static HANDLE g_stop = NULL, g_wake = NULL, g_thread = NULL;
static WCHAR  g_pipe_name[96];

bool control_paused(void) { return g_paused != 0; }

void control_set_paused(bool paused) {
    InterlockedExchange(&g_paused, paused ? 1 : 0);
    log_msg("INFO", paused ? "monitoring turned OFF (sampling and triggers paused)" : "monitoring turned ON");
    if (g_wake) SetEvent(g_wake);
}

void control_request_capture(void) {
    InterlockedExchange(&g_manual, 1);
    if (g_wake) SetEvent(g_wake);
}
bool control_take_capture(void) { return InterlockedExchange(&g_manual, 0) != 0; }

void control_set_capturing(bool on) { InterlockedExchange(&g_capturing, on ? 1 : 0); }
void control_set_hotkey_registered(bool ok) { InterlockedExchange(&g_hotkey_ok, ok ? 1 : 0); }

void control_capture_done(const WCHAR *dir, bool ok) {
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
        wft_wsnprintf(g_last_dir, WFT_PATH_CCH, L"%ls", (dir && dir[0]) ? dir : L"");
        LeaveCriticalSection(&g_lock);
    }
    InterlockedExchange(&g_last_ok, ok ? 1 : 0);
    InterlockedIncrement(ok ? &g_captures : &g_failures);        /* last: the client sees the counter after the folder */
}

/* ---- replies ---- */

static void json_escape(const WCHAR *w, char *out, size_t cap) {
    char utf8[WFT_PATH_CCH * 3];
    wft_wcs_to_utf8(w ? w : L"", utf8, sizeof utf8);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n + 8 < cap; ++p) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p < 0x20)          { n += (size_t)wft_snprintf(out + n, cap - n, "\\u%04x", *p); }
        else                         { out[n++] = (char)*p; }
    }
    out[n] = 0;
}

static size_t reply_status(char *out, size_t cap, bool ok, const char *error) {
    static char last[WFT_PATH_CCH * 6], reports[WFT_PATH_CCH * 6];     /* single pipe thread */
    WCHAR dir[WFT_PATH_CCH];
    EnterCriticalSection(&g_lock);
    wft_wsnprintf(dir, WFT_PATH_CCH, L"%ls", g_last_dir);
    LeaveCriticalSection(&g_lock);
    json_escape(dir, last, sizeof last);
    json_escape(g_reports, reports, sizeof reports);
    int n = wft_snprintf(out, cap,
        "{\"ok\":%s,\"error\":\"%s\",\"proto\":%d,\"version\":\"%s\",\"pid\":%lu,"
        "\"paused\":%s,\"capturing\":%s,\"hotkey\":%s,\"captures\":%ld,\"failures\":%ld,"
        "\"last_ok\":%s,\"last_report\":\"%s\",\"reports_dir\":\"%s\"}\n",
        ok ? "true" : "false", error ? error : "", WFT_CONTROL_PROTO, WFT_VERSION, (unsigned long)GetCurrentProcessId(),
        g_paused ? "true" : "false", g_capturing ? "true" : "false", g_hotkey_ok ? "true" : "false",
        (long)g_captures, (long)g_failures, g_last_ok ? "true" : "false", last, reports);
    return n > 0 ? (size_t)n : 0;
}

static size_t handle_command(const char *cmd, char *out, size_t cap) {
    if (!strcmp(cmd, "STATUS"))  return reply_status(out, cap, true, NULL);
    if (!strcmp(cmd, "PAUSE"))   { control_set_paused(true);  return reply_status(out, cap, true, NULL); }
    if (!strcmp(cmd, "RESUME"))  { control_set_paused(false); return reply_status(out, cap, true, NULL); }
    if (!strcmp(cmd, "CAPTURE")) {
        if (g_paused)    return reply_status(out, cap, false, "monitoring is off");
        if (g_capturing) return reply_status(out, cap, false, "a report is already being written");
        log_msg("INFO", "manual capture requested through the control pipe");
        control_request_capture();
        return reply_status(out, cap, true, NULL);
    }
    if (!strcmp(cmd, "EXIT")) {
        log_msg("INFO", "exit requested through the control pipe");
        size_t n = reply_status(out, cap, true, NULL);
        if (g_stop) SetEvent(g_stop);
        return n;
    }
    return reply_status(out, cap, false, "unknown command");
}

/* ---- server thread: overlapped I/O so that the stop event can interrupt every wait ---- */

static bool wait_io(HANDLE pipe, OVERLAPPED *ov, DWORD timeout, DWORD *bytes, bool *stopped) {
    HANDLE h[2] = { ov->hEvent, g_stop };
    DWORD w = WaitForMultipleObjects(2, h, FALSE, timeout);
    if (w == WAIT_OBJECT_0 && GetOverlappedResult(pipe, ov, bytes, FALSE)) return true;
    *stopped = (w == WAIT_OBJECT_0 + 1);
    CancelIoEx(pipe, ov);
    GetOverlappedResult(pipe, ov, bytes, TRUE);          /* the OVERLAPPED must outlive the cancelled operation */
    return false;
}

static void serve_client(HANDLE pipe, HANDLE ev) {
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = ev;
    char req[128]; DWORD got = 0; bool stopped = false;
    ResetEvent(ev);
    BOOL r = ReadFile(pipe, req, sizeof req - 1, NULL, &ov);
    if (!r && GetLastError() != ERROR_IO_PENDING) return;
    if (!wait_io(pipe, &ov, IO_TIMEOUT_MS, &got, &stopped)) return;
    req[got] = 0;
    size_t l = strlen(req);
    while (l && (req[l - 1] == '\n' || req[l - 1] == '\r' || req[l - 1] == ' ')) req[--l] = 0;

    static char reply[WFT_PATH_CCH * 12 + 512];
    size_t n = handle_command(req, reply, sizeof reply);
    if (!n) return;
    ResetEvent(ev); memset(&ov, 0, sizeof ov); ov.hEvent = ev;
    DWORD put = 0;
    r = WriteFile(pipe, reply, (DWORD)n, NULL, &ov);
    if (!r && GetLastError() != ERROR_IO_PENDING) return;
    wait_io(pipe, &ov, IO_TIMEOUT_MS, &put, &stopped);
    FlushFileBuffers(pipe);
}

static DWORD WINAPI control_thread(LPVOID unused) {
    (void)unused;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) return 1;
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        SECURITY_ATTRIBUTES sa; bool have_sa = plat_private_security(&sa);
        HANDLE pipe = CreateNamedPipeW(g_pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 8192, 8192, 0, have_sa ? &sa : NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            log_msg("WARN", "control pipe unavailable (error %lu); retrying", (unsigned long)GetLastError());
            if (WaitForSingleObject(g_stop, 2000) == WAIT_OBJECT_0) break;
            continue;
        }
        OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = ev; ResetEvent(ev);
        BOOL c = ConnectNamedPipe(pipe, &ov);
        DWORD err = c ? 0 : GetLastError();
        bool connected = c || err == ERROR_PIPE_CONNECTED;
        if (!connected && err == ERROR_IO_PENDING) {
            DWORD dummy = 0; bool stopped = false;
            HANDLE h[2] = { ev, g_stop };
            if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) stopped = true;
            if (stopped) { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &dummy, TRUE); CloseHandle(pipe); break; }
            connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != 0;
        }
        if (connected) serve_client(pipe, ev);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    CloseHandle(ev);
    return 0;
}

bool control_start(HANDLE stop_event, HANDLE wake_event, const ControlOptions *opt) {
    g_stop = stop_event; g_wake = wake_event;
    wft_wsnprintf(g_reports, WFT_PATH_CCH, L"%ls", opt->reports_dir ? opt->reports_dir : L"");
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) session = 0;
    wft_wsnprintf(g_pipe_name, sizeof g_pipe_name / sizeof g_pipe_name[0], PIPE_PREFIX L"%lu", (unsigned long)session);
    g_thread = CreateThread(NULL, 0, control_thread, NULL, 0, NULL);   /* default priority: it must answer while the system is loaded */
    if (!g_thread) { log_msg("WARN", "control thread could not start: WinFreezeTrace Manager cannot connect"); return false; }
    log_msg_w("INFO", "control pipe: %s", g_pipe_name);
    return true;
}

void control_stop(void) {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = NULL; }
}
