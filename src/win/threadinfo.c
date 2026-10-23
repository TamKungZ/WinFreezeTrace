#include "threadinfo.h"

#define MAX_THREADS 2048

typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);
static ULONGLONG ft64(FILETIME f) { ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime; return u.QuadPart; }

void threads_begin(DWORD pid, ThreadSnap *ts) {
    memset(ts, 0, sizeof *ts);
    ts->base_ms = plat_qpc_ms();
    if (!pid) return;
    ts->tids = (DWORD *)calloc(MAX_THREADS, sizeof(DWORD));
    ts->base_pri = (LONG *)calloc(MAX_THREADS, sizeof(LONG));
    ts->cpu100ns = (ULONGLONG *)calloc(MAX_THREADS, sizeof(ULONGLONG));
    if (!ts->tids || !ts->base_pri || !ts->cpu100ns) { threads_free(ts); return; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; memset(&te, 0, sizeof te); te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (ts->n >= MAX_THREADS) { ts->truncated = true; break; }
            ts->tids[ts->n] = te.th32ThreadID; ts->base_pri[ts->n] = te.tpBasePri;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (h) {
                FILETIME c, e, k, u;
                if (GetThreadTimes(h, &c, &e, &k, &u)) ts->cpu100ns[ts->n] = ft64(k) + ft64(u);
                CloseHandle(h);
            }
            ++ts->n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void threads_free(ThreadSnap *ts) { free(ts->tids); free(ts->base_pri); free(ts->cpu100ns); memset(ts, 0, sizeof *ts); }

int threads_finish_write(const WCHAR *dir, DWORD pid, DWORD gui_tid, const ThreadSnap *ts, HotThread *top, int max_top) {
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\threads.csv", dir);
    FILE *f = plat_fopen_w(path); if (!f) return 0;
    fputs("tid,is_gui_thread,name,base_priority,current_priority,kernel_ms,user_ms,cpu_one_core_pct_in_window,created_utc_filetime\r\n", f);
    (void)pid;
    double window_ms = plat_qpc_ms() - ts->base_ms; if (window_ms < 1.0) window_ms = 1.0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    GetThreadDescription_t gtd = k32 ? (GetThreadDescription_t)(void *)GetProcAddress(k32, "GetThreadDescription") : NULL;   /* Win10 1607+ */
    int nt = 0;
    for (int i = 0; i < ts->n; ++i) {
        double kms = WFT_NA, ums = WFT_NA, pct = WFT_NA; ULONGLONG created = 0; int pri = THREAD_PRIORITY_ERROR_RETURN;
        WCHAR name[64] = {0};
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ts->tids[i]);
        if (h) {
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                created = ft64(c); kms = (double)ft64(k) / 1e4; ums = (double)ft64(u) / 1e4;
                ULONGLONG cur = ft64(k) + ft64(u);
                if (ts->cpu100ns[i] && cur >= ts->cpu100ns[i]) pct = (double)(cur - ts->cpu100ns[i]) / (window_ms * 10000.0) * 100.0;
            }
            pri = GetThreadPriority(h);
            if (gtd) { PWSTR d = NULL; if (SUCCEEDED(gtd(h, &d)) && d) { wft_wsnprintf(name, 64, L"%ls", d); LocalFree(d); } }
            CloseHandle(h);
        }
        bool gui = ts->tids[i] == gui_tid;
        fprintf(f, "%lu,%d,", (unsigned long)ts->tids[i], gui ? 1 : 0); wft_csv_text(f, name);
        fprintf(f, ",%ld,%d,%.3f,%.3f,%.1f,%llu\r\n", (long)ts->base_pri[i], pri, kms, ums, pct, (unsigned long long)created);

        if (WFT_HAS(pct) && pct >= 1.0 && max_top > 0) {          /* keep the hottest few for the summary */
            int pos = 0; while (pos < nt && top[pos].pct >= pct) ++pos;
            if (pos < max_top) {
                for (int j = (nt < max_top ? nt : max_top - 1); j > pos; --j) top[j] = top[j - 1];
                top[pos].tid = ts->tids[i]; top[pos].pct = pct; top[pos].gui = gui;
                wft_ascii_from_wide(top[pos].name, sizeof top[pos].name, name);
                if (nt < max_top) ++nt;
            }
        }
    }
    fclose(f);
    return nt;
}

/* wait chains */

static const char *wct_type_name(WCT_OBJECT_TYPE t) {
    switch (t) {
        case WctCriticalSectionType: return "CriticalSection";
        case WctSendMessageType: return "SendMessage";
        case WctMutexType: return "Mutex";
        case WctAlpcType: return "ALPC";
        case WctComType: return "COM";
        case WctThreadWaitType: return "ThreadWait";
        case WctProcessWaitType: return "ProcessWait";
        case WctThreadType: return "Thread";
        case WctComActivationType: return "COMActivation";
        case WctSocketIoType: return "SocketIO";
        case WctSmbIoType: return "SMBIO";
        default: return "Unknown";
    }
}

/* Numeric on purpose: values 1..10 of WCT_OBJECT_STATUS. */
static const char *wct_status_name(int s) {
    static const char *const n[] = { "?", "NoAccess", "Running", "Blocked", "PidOnly", "PidOnlyRpcss", "Owned", "NotOwned", "Abandoned", "Unknown", "Error" };
    return (s >= 1 && s <= 10) ? n[s] : "?";
}

static void append(char *dst, size_t cch, const char *fmt, ...) {
    size_t l = strlen(dst);
    if (l + 1 >= cch) return;
    va_list ap; va_start(ap, fmt); vsnprintf(dst + l, cch - l, fmt, ap); va_end(ap);
    dst[cch - 1] = 0;
}

static void describe_chain(const WAITCHAIN_NODE_INFO *nodes, DWORD count, DWORD target_pid, char *out, size_t cch, bool *cross, DWORD *blocker) {
    out[0] = 0; *cross = false; *blocker = 0;
    for (DWORD i = 0; i < count; ++i) {
        const WAITCHAIN_NODE_INFO *n = &nodes[i];
        if (i) append(out, cch, " -> ");
        if (n->ObjectType == WctThreadType) {
            DWORD p = n->ThreadObject.ProcessId;
            if (p && p != target_pid) {
                WCHAR exe[WFT_PATH_CCH]; char nm[64] = "?";
                if (plat_pid_exe(p, exe, WFT_PATH_CCH)) wft_ascii_from_wide(nm, sizeof nm, wft_base_name(exe));
                append(out, cch, "thread %lu (%s pid %lu)", (unsigned long)n->ThreadObject.ThreadId, nm, (unsigned long)p);
                *cross = true; *blocker = p;
            } else append(out, cch, "thread %lu", (unsigned long)n->ThreadObject.ThreadId);
        } else {
            char nm[48] = "";
            if (n->LockObject.ObjectName[0]) wft_ascii_from_wide(nm, sizeof nm, n->LockObject.ObjectName);
            append(out, cch, "%s%s%s%s", wct_type_name(n->ObjectType), nm[0] ? " '" : "", nm, nm[0] ? "'" : "");
        }
    }
}

void wait_chains_write(const WCHAR *dir, const ThreadSnap *ts, DWORD pid, DWORD gui_tid, DWORD budget_ms, WaitInfo *wi) {
    memset(wi, 0, sizeof *wi);
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\waitchains.txt", dir);
    FILE *f = plat_fopen_w(path); if (!f) return;
    fprintf(f, "Wait Chain Traversal for PID %lu (GUI thread %lu is listed first)\r\n", (unsigned long)pid, (unsigned long)gui_tid);
    fputs("A chain with only the thread itself means it waits on something Windows cannot attribute (idle, message loop, event).\r\n"
          "A chain with more nodes shows what the thread is blocked behind. cycle=YES indicates a deadlock.\r\n", f);
    if (!pid || !ts->n) { fputs("No foreground target process.\r\n", f); fclose(f); return; }
    HWCT session = OpenThreadWaitChainSession(0, NULL);
    if (!session) { fprintf(f, "OpenThreadWaitChainSession failed: %lu\r\n", (unsigned long)GetLastError()); fclose(f); return; }
    wi->ok = true;
    ULONGLONG t0 = GetTickCount64();
    WAITCHAIN_NODE_INFO *nodes = (WAITCHAIN_NODE_INFO *)malloc(sizeof(WAITCHAIN_NODE_INFO) * WCT_MAX_NODE_COUNT);
    if (!nodes) { CloseThreadWaitChainSession(session); fclose(f); return; }
    for (int pass = 0; pass < 2; ++pass) {                      /* pass 0: GUI thread only, pass 1: the rest */
        for (int i = 0; i < ts->n; ++i) {
            DWORD tid = ts->tids[i];
            if ((pass == 0) != (tid == gui_tid)) continue;
            if (pass == 1 && GetTickCount64() - t0 > budget_ms) { wi->budget_exhausted = true; break; }
            DWORD count = WCT_MAX_NODE_COUNT; BOOL cycle = FALSE;
            memset(nodes, 0, sizeof(WAITCHAIN_NODE_INFO) * WCT_MAX_NODE_COUNT);
            if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, tid, &count, nodes, &cycle)) {
                fprintf(f, "\r\nThread %lu%s: GetThreadWaitChain failed: %lu\r\n", (unsigned long)tid, tid == gui_tid ? " [GUI]" : "", (unsigned long)GetLastError());
                continue;
            }
            ++wi->scanned;
            if (count > 1) ++wi->in_chain;
            if (cycle) wi->any_cycle = true;
            fprintf(f, "\r\nThread %lu%s  cycle=%s  nodes=%lu\r\n", (unsigned long)tid, tid == gui_tid ? " [GUI]" : "", cycle ? "YES" : "no", (unsigned long)count);
            for (DWORD k = 0; k < count && k < WCT_MAX_NODE_COUNT; ++k) {
                const WAITCHAIN_NODE_INFO *n = &nodes[k];
                fprintf(f, "  [%lu] %-16s status=%s", (unsigned long)k, wct_type_name(n->ObjectType), wct_status_name((int)n->ObjectStatus));
                if (n->ObjectType == WctThreadType) {
                    fprintf(f, " pid=%lu tid=%lu wait_ms=%lu switches=%lu", (unsigned long)n->ThreadObject.ProcessId, (unsigned long)n->ThreadObject.ThreadId,
                            (unsigned long)n->ThreadObject.WaitTime, (unsigned long)n->ThreadObject.ContextSwitches);
                    if (n->ThreadObject.ProcessId && n->ThreadObject.ProcessId != pid) {
                        WCHAR exe[WFT_PATH_CCH];
                        if (plat_pid_exe(n->ThreadObject.ProcessId, exe, WFT_PATH_CCH)) { fputs(" exe=", f); wft_write_utf8(f, wft_base_name(exe)); }
                    }
                } else if (n->LockObject.ObjectName[0]) { fputs(" name=", f); wft_write_utf8(f, n->LockObject.ObjectName); }
                fputs("\r\n", f);
            }
            if (tid == gui_tid && count > 1)
                describe_chain(nodes, count, pid, wi->ui_blocker, sizeof wi->ui_blocker, &wi->ui_cross_process, &wi->blocker_pid);
        }
    }
    if (wi->budget_exhausted) fprintf(f, "\r\n(Stopped after %lu ms; remaining threads were not examined.)\r\n", (unsigned long)budget_ms);
    free(nodes);
    CloseThreadWaitChainSession(session);
    fclose(f);
}

void modules_write(const WCHAR *dir, DWORD pid, const Config *cfg) {
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\modules.csv", dir);
    FILE *f = plat_fopen_w(path); if (!f) return;
    fputs("base,size,module,path\r\n", f);
    HANDLE snap = pid ? CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid) : INVALID_HANDLE_VALUE;
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me; memset(&me, 0, sizeof me); me.dwSize = sizeof me;
        if (Module32FirstW(snap, &me)) {
            do {
                fprintf(f, "0x%p,%lu,", (void *)me.modBaseAddr, (unsigned long)me.modBaseSize);
                wft_csv_text(f, me.szModule); fputc(',', f);
                WCHAR p[MAX_PATH + 8]; wft_wsnprintf(p, MAX_PATH + 8, L"%ls", me.szExePath);
                if (cfg->redact) wft_redact_user_path(p, MAX_PATH + 8);
                wft_csv_text(f, p); fputs("\r\n", f);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    fclose(f);
}
