#include "foreground.h"

const char *probe_status_name(int s) {
    switch (s) {
        case PROBE_NONE: return "no foreground window";
        case PROBE_OK: return "responded";
        case PROBE_TIMEOUT: return "TIMEOUT (message not processed)";
        case PROBE_HUNG_FLAG: return "HUNG (Windows ghost-window state)";
        case PROBE_DENIED: return "blocked by UIPI (elevated target; not a hang)";
        case PROBE_INVALID: return "window vanished while probing";
        case PROBE_SELF: return "foreground is WinFreezeTrace";
        case PROBE_SECURE_DESKTOP: return "secure desktop (UAC/lock screen)";
        default: return "unknown";
    }
}

/* False on the UAC prompt / lock screen: those run on a desktop we cannot open. */
static bool input_desktop_is_default(void) {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return false;
    WCHAR name[64] = {0}; DWORD n = 0;
    BOOL ok = GetUserObjectInformationW(d, UOI_NAME, name, sizeof name, &n);
    CloseDesktop(d);
    return ok && _wcsicmp(name, L"Default") == 0;
}

void foreground_probe(ForegroundInfo *fi, DWORD timeout_ms, DWORD self_pid) {
    memset(fi, 0, sizeof *fi);
    fi->status = PROBE_NONE; fi->probe_ms = WFT_NA;
    if (!input_desktop_is_default()) { fi->status = PROBE_SECURE_DESKTOP; return; }
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;
    fi->hwnd = hwnd;
    fi->tid = GetWindowThreadProcessId(hwnd, &fi->pid);
    GetWindowTextW(hwnd, fi->title, (int)(sizeof fi->title / sizeof fi->title[0]));
    GetClassNameW(hwnd, fi->cls, (int)(sizeof fi->cls / sizeof fi->cls[0]));
    if (fi->pid) plat_pid_exe(fi->pid, fi->path, WFT_PATH_CCH);
    if (fi->pid == self_pid) { fi->status = PROBE_SELF; return; }

    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    double t0 = plat_qpc_ms();
    LRESULT r = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, timeout_ms, &result);
    DWORD err = GetLastError();
    fi->probe_ms = plat_qpc_ms() - t0;
    if (r != 0) { fi->status = PROBE_OK; return; }
    if (err == ERROR_TIMEOUT) fi->status = PROBE_TIMEOUT;
    else if (err == ERROR_ACCESS_DENIED) { fi->status = PROBE_DENIED; fi->probe_ms = WFT_NA; }
    else if (err == ERROR_INVALID_WINDOW_HANDLE) { fi->status = PROBE_INVALID; fi->probe_ms = WFT_NA; }
    else fi->status = IsHungAppWindow(hwnd) ? PROBE_HUNG_FLAG : PROBE_INVALID;   /* SMTO_ABORTIFHUNG may fail without ERROR_TIMEOUT */
}

void foreground_to_sample(const ForegroundInfo *fi, Sample *s) {
    s->fg_pid = fi->pid; s->fg_tid = fi->tid;
    s->probe_status = fi->status; s->probe_ms = fi->probe_ms;
    wft_wsnprintf(s->fg_exe, sizeof s->fg_exe / sizeof s->fg_exe[0], L"%ls", wft_base_name(fi->path));
    wft_wsnprintf(s->fg_class, sizeof s->fg_class / sizeof s->fg_class[0], L"%ls", fi->cls);
    wft_wsnprintf(s->fg_title, sizeof s->fg_title / sizeof s->fg_title[0], L"%ls", fi->title);
}

static void put(FILE *f, const char *label, const WCHAR *v) { fprintf(f, "%s: ", label); wft_write_utf8(f, v); fputs("\r\n", f); }

void foreground_write_details(const WCHAR *dir, const ForegroundInfo *fi, const Config *cfg) {
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\foreground.txt", dir);
    FILE *f = plat_fopen_w(path); if (!f) return;
    fputs("Foreground application at trigger time\r\n=====================================\r\n", f);
    fprintf(f, "PID/TID: %lu / %lu\r\nProbe result: %s", (unsigned long)fi->pid, (unsigned long)fi->tid, probe_status_name(fi->status));
    if (WFT_HAS(fi->probe_ms)) fprintf(f, " (%.1f ms)", fi->probe_ms);
    fputs("\r\n", f);
    WCHAR p[WFT_PATH_CCH]; wft_wsnprintf(p, WFT_PATH_CCH, L"%ls", fi->path);
    if (cfg->redact) wft_redact_user_path(p, WFT_PATH_CCH);
    put(f, "Executable", p);
    put(f, "Window class", fi->cls);
    put(f, "Window title", cfg->redact ? L"<redacted>" : fi->title);

    if (fi->hwnd && IsWindow(fi->hwnd)) {
        RECT r; MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (GetWindowRect(fi->hwnd, &r)) {
            fprintf(f, "Window rect: %ld,%ld - %ld,%ld (%ldx%ld)\r\n", r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
            HMONITOR mon = MonitorFromWindow(fi->hwnd, MONITOR_DEFAULTTONEAREST);
            if (mon && GetMonitorInfoW(mon, &mi))
                fprintf(f, "Covers whole monitor (fullscreen/borderless): %s\r\n",
                        (r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom) ? "yes" : "no");
        }
        fprintf(f, "Visible: %s, minimised: %s, Windows hung-window flag: %s\r\n", IsWindowVisible(fi->hwnd) ? "yes" : "no",
                IsIconic(fi->hwnd) ? "yes" : "no", IsHungAppWindow(fi->hwnd) ? "YES" : "no");
    }
    LASTINPUTINFO li; li.cbSize = sizeof li;
    if (GetLastInputInfo(&li)) fprintf(f, "User idle time: %.1f s (long idle means a stall may have gone unnoticed)\r\n", (double)(GetTickCount() - li.dwTime) / 1000.0);

    HANDLE h = fi->pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fi->pid) : NULL;
    if (h) {
        DWORD sess = 0; if (ProcessIdToSessionId(fi->pid, &sess)) fprintf(f, "Session: %lu\r\n", (unsigned long)sess);
        BOOL wow = FALSE; if (IsWow64Process(h, &wow)) fprintf(f, "32-bit process on 64-bit Windows: %s\r\n", wow ? "yes" : "no");
        HANDLE tok = NULL;
        if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION el; DWORD n = 0;
            if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n)) fprintf(f, "Elevated (admin): %s\r\n", el.TokenIsElevated ? "yes" : "no");
            CloseHandle(tok);
        }
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            FILETIME now; GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER cc, nn, kk, uu; cc.LowPart = c.dwLowDateTime; cc.HighPart = c.dwHighDateTime; nn.LowPart = now.dwLowDateTime; nn.HighPart = now.dwHighDateTime;
            kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime; uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
            fprintf(f, "Process age: %.1f min; CPU time: %.1f s user, %.1f s kernel\r\n", (double)(nn.QuadPart - cc.QuadPart) / 6e8,
                    (double)uu.QuadPart / 1e7, (double)kk.QuadPart / 1e7);
        }
        PROCESS_MEMORY_COUNTERS_EX pmc; memset(&pmc, 0, sizeof pmc); pmc.cb = sizeof pmc;
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
            fprintf(f, "Working set: %.1f MB (peak %.1f), private: %.1f MB, page faults: %lu\r\n", (double)pmc.WorkingSetSize / 1048576.0,
                    (double)pmc.PeakWorkingSetSize / 1048576.0, (double)pmc.PrivateUsage / 1048576.0, (unsigned long)pmc.PageFaultCount);
        DWORD hc = 0; if (GetProcessHandleCount(h, &hc)) fprintf(f, "Handles: %lu\r\n", (unsigned long)hc);
        IO_COUNTERS io; if (GetProcessIoCounters(h, &io))
            fprintf(f, "I/O so far: %llu reads (%.1f MB), %llu writes (%.1f MB)\r\n", (unsigned long long)io.ReadOperationCount,
                    (double)io.ReadTransferCount / 1048576.0, (unsigned long long)io.WriteOperationCount, (double)io.WriteTransferCount / 1048576.0);
        CloseHandle(h);
    } else if (fi->pid) fputs("Process details unavailable (access denied: protected or elevated process).\r\n", f);
    fclose(f);
}
