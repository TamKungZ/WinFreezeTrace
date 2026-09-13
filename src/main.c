#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <winevt.h>
#include <wct.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib")

#define APP_NAME L"WinFreezeTrace"
#define MAX_RING 600
#define MAX_TITLE 512
#define MAX_PATH_W 1024
#define MAX_PROCS 4096
#define MAX_EVENT_XML 262144

#define EVENTFLAG_WHEA      0x0001
#define EVENTFLAG_STORAGE   0x0002
#define EVENTFLAG_DISPLAY   0x0004
#define EVENTFLAG_APPHANG   0x0008
#define EVENTFLAG_POWER     0x0010
#define EVENTFLAG_NTFS      0x0020

typedef struct Config {
    DWORD interval_ms;
    DWORD history_seconds;
    DWORD stall_ms;
    DWORD hang_timeout_ms;
    DWORD hang_samples;
    DWORD cooldown_seconds;
    BOOL make_dump;
    BOOL background;
    WCHAR report_root[MAX_PATH_W];
} Config;

typedef struct Metrics {
    double cpu;
    double dpc;
    double interrupt;
    double disk_queue;
    double disk_read_bps;
    double disk_write_bps;
    double commit_pct;
    double proc_queue;
    DWORD mem_load;
    uint64_t avail_mb;
} Metrics;

typedef struct Sample {
    SYSTEMTIME ts;
    int wake_late_ms;
    Metrics m;
    DWORD fg_pid;
    DWORD fg_tid;
    BOOL fg_hung;
    WCHAR fg_path[MAX_PATH_W];
    WCHAR fg_title[MAX_TITLE];
    WCHAR fg_class[128];
} Sample;

typedef struct PdhState {
    PDH_HQUERY q;
    PDH_HCOUNTER cpu;
    PDH_HCOUNTER dpc;
    PDH_HCOUNTER intr;
    PDH_HCOUNTER diskq;
    PDH_HCOUNTER readbps;
    PDH_HCOUNTER writebps;
    PDH_HCOUNTER commit;
    PDH_HCOUNTER procq;
    BOOL ok;
} PdhState;

typedef struct ProcBase {
    DWORD pid;
    ULONGLONG cpu100ns;
} ProcBase;

typedef struct ProcRow {
    DWORD pid;
    DWORD ppid;
    double cpu_system_pct;
    double cpu_one_core_pct;
    SIZE_T working_set;
    SIZE_T private_bytes;
    ULONGLONG read_bytes;
    ULONGLONG write_bytes;
    DWORD handles;
    WCHAR path[MAX_PATH_W];
} ProcRow;

typedef enum TriggerReason {
    TRIGGER_SCHEDULER_STALL = 1,
    TRIGGER_APP_HANG = 2,
    TRIGGER_MANUAL = 3
} TriggerReason;

static volatile LONG g_manual_trigger = 0;
static LARGE_INTEGER g_qpf;

static double qpc_ms(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

static void write_utf8(FILE *f, const WCHAR *s) {
    if (!s) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 1) return;
    char *buf = (char*)malloc((size_t)n);
    if (!buf) return;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, n, NULL, NULL);
    fwrite(buf, 1, (size_t)n - 1, f);
    free(buf);
}

static void csv_w(FILE *f, const WCHAR *s) {
    fputc('"', f);
    if (s) {
        for (const WCHAR *p = s; *p; ++p) {
            if (*p == L'"') {
                fputs("\"\"", f);
            } else {
                WCHAR tmp[2] = {*p, 0};
                write_utf8(f, tmp);
            }
        }
    }
    fputc('"', f);
}

static BOOL ensure_dir(const WCHAR *path) {
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static void default_report_root(WCHAR *out, size_t cch) {
    WCHAR base[MAX_PATH_W] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, base))) {
        _snwprintf(out, cch, L"%ls\\WinFreezeTrace\\reports", base);
        WCHAR parent[MAX_PATH_W];
        _snwprintf(parent, _countof(parent), L"%ls\\WinFreezeTrace", base);
        ensure_dir(parent);
        ensure_dir(out);
    } else {
        GetCurrentDirectoryW((DWORD)cch, out);
    }
}

static const WCHAR* base_name(const WCHAR *path) {
    const WCHAR *p = path, *last = path;
    while (p && *p) {
        if (*p == L'\\' || *p == L'/') last = p + 1;
        ++p;
    }
    return last;
}

static BOOL get_process_path(DWORD pid, WCHAR *out, DWORD cch) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    DWORD n = cch;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);
    return ok;
}

static void get_foreground(DWORD *pid, DWORD *tid, WCHAR *title, size_t title_cch, WCHAR *wnd_class, size_t class_cch, WCHAR *path, size_t path_cch, BOOL *hung, DWORD timeout_ms) {
    *pid = 0; *tid = 0; *hung = FALSE;
    title[0] = 0; wnd_class[0] = 0; path[0] = 0;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;
    *tid = GetWindowThreadProcessId(hwnd, pid);
    GetWindowTextW(hwnd, title, (int)title_cch);
    GetClassNameW(hwnd, wnd_class, (int)class_cch);
    if (*pid) get_process_path(*pid, path, (DWORD)path_cch);

    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    LRESULT r = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        timeout_ms, &result);
    if (r == 0 && GetLastError() == ERROR_TIMEOUT) *hung = TRUE;
}

static double pdh_value(PDH_HCOUNTER c) {
    if (!c) return -1.0;
    PDH_FMT_COUNTERVALUE v;
    DWORD type = 0;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, &type, &v) != ERROR_SUCCESS) return -1.0;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return -1.0;
    return v.doubleValue;
}

static void pdh_add(PDH_HQUERY q, const WCHAR *path, PDH_HCOUNTER *out) {
    *out = NULL;
    if (PdhAddEnglishCounterW(q, path, 0, out) != ERROR_SUCCESS) {
        PdhAddCounterW(q, path, 0, out);
    }
}

static BOOL pdh_init(PdhState *p) {
    ZeroMemory(p, sizeof(*p));
    if (PdhOpenQueryW(NULL, 0, &p->q) != ERROR_SUCCESS) return FALSE;
    pdh_add(p->q, L"\\Processor(_Total)\\% Processor Time", &p->cpu);
    pdh_add(p->q, L"\\Processor(_Total)\\% DPC Time", &p->dpc);
    pdh_add(p->q, L"\\Processor(_Total)\\% Interrupt Time", &p->intr);
    pdh_add(p->q, L"\\PhysicalDisk(_Total)\\Avg. Disk Queue Length", &p->diskq);
    pdh_add(p->q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &p->readbps);
    pdh_add(p->q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &p->writebps);
    pdh_add(p->q, L"\\Memory\\% Committed Bytes In Use", &p->commit);
    pdh_add(p->q, L"\\System\\Processor Queue Length", &p->procq);
    PdhCollectQueryData(p->q);
    p->ok = TRUE;
    return TRUE;
}

static void pdh_close(PdhState *p) {
    if (p->q) PdhCloseQuery(p->q);
    ZeroMemory(p, sizeof(*p));
}

static void collect_metrics(PdhState *p, Metrics *m) {
    ZeroMemory(m, sizeof(*m));
    m->cpu = m->dpc = m->interrupt = m->disk_queue = -1.0;
    m->disk_read_bps = m->disk_write_bps = m->commit_pct = m->proc_queue = -1.0;
    if (p->ok && PdhCollectQueryData(p->q) == ERROR_SUCCESS) {
        m->cpu = pdh_value(p->cpu);
        m->dpc = pdh_value(p->dpc);
        m->interrupt = pdh_value(p->intr);
        m->disk_queue = pdh_value(p->diskq);
        m->disk_read_bps = pdh_value(p->readbps);
        m->disk_write_bps = pdh_value(p->writebps);
        m->commit_pct = pdh_value(p->commit);
        m->proc_queue = pdh_value(p->procq);
    }
    MEMORYSTATUSEX ms;
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m->mem_load = ms.dwMemoryLoad;
        m->avail_mb = ms.ullAvailPhys / (1024ull * 1024ull);
    }
}

static const char *trigger_name(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return "scheduler_stall";
        case TRIGGER_APP_HANG: return "foreground_app_hang";
        case TRIGGER_MANUAL: return "manual_hotkey";
        default: return "unknown";
    }
}

static const WCHAR *trigger_name_w(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return L"scheduler_stall";
        case TRIGGER_APP_HANG: return L"foreground_app_hang";
        case TRIGGER_MANUAL: return L"manual_hotkey";
        default: return L"unknown";
    }
}

static DWORD WINAPI hotkey_thread(LPVOID unused) {
    (void)unused;
    if (!RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12)) return 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == 1) {
            InterlockedExchange(&g_manual_trigger, 1);
        }
    }
    UnregisterHotKey(NULL, 1);
    return 0;
}

static void make_report_dir(const Config *cfg, const SYSTEMTIME *st, TriggerReason reason, WCHAR *out, size_t cch) {
    _snwprintf(out, cch,
        L"%ls\\%04u%02u%02u_%02u%02u%02u_%ls",
        cfg->report_root,
        st->wYear, st->wMonth, st->wDay,
        st->wHour, st->wMinute, st->wSecond,
        trigger_name_w(reason));
    ensure_dir(out);
}

static void write_timeline(const WCHAR *dir, const Sample *ring, int ring_count, int ring_head) {
    WCHAR path[MAX_PATH_W];
    _snwprintf(path, _countof(path), L"%ls\\timeline.csv", dir);
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fputs("time,wake_late_ms,cpu_pct,dpc_pct,interrupt_pct,disk_queue,disk_read_Bps,disk_write_Bps,commit_pct,mem_load_pct,avail_mem_MB,processor_queue,fg_pid,fg_tid,fg_hung,fg_exe,fg_class,fg_title\r\n", f);
    int start = (ring_head - ring_count + MAX_RING) % MAX_RING;
    for (int i = 0; i < ring_count; ++i) {
        const Sample *s = &ring[(start + i) % MAX_RING];
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%d,%.2f,%.2f,%.2f,%.3f,%.0f,%.0f,%.2f,%lu,%llu,%.2f,%lu,%lu,%d,",
            s->ts.wYear, s->ts.wMonth, s->ts.wDay,
            s->ts.wHour, s->ts.wMinute, s->ts.wSecond, s->ts.wMilliseconds,
            s->wake_late_ms, s->m.cpu, s->m.dpc, s->m.interrupt,
            s->m.disk_queue, s->m.disk_read_bps, s->m.disk_write_bps,
            s->m.commit_pct, (unsigned long)s->m.mem_load,
            (unsigned long long)s->m.avail_mb, s->m.proc_queue,
            (unsigned long)s->fg_pid, (unsigned long)s->fg_tid, s->fg_hung ? 1 : 0);
        csv_w(f, base_name(s->fg_path)); fputc(',', f);
        csv_w(f, s->fg_class); fputc(',', f);
        csv_w(f, s->fg_title); fputs("\r\n", f);
    }
    fclose(f);
}

static int cmp_proc_cpu(const void *a, const void *b) {
    const ProcRow *pa = (const ProcRow*)a, *pb = (const ProcRow*)b;
    if (pa->cpu_system_pct < pb->cpu_system_pct) return 1;
    if (pa->cpu_system_pct > pb->cpu_system_pct) return -1;
    return 0;
}

static int collect_proc_baseline(ProcBase *base, int maxn) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe)); pe.dwSize = sizeof(pe);
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n >= maxn) break;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                FILETIME c,e,k,u;
                if (GetProcessTimes(h, &c, &e, &k, &u)) {
                    ULARGE_INTEGER ku, uu; ku.LowPart=k.dwLowDateTime; ku.HighPart=k.dwHighDateTime; uu.LowPart=u.dwLowDateTime; uu.HighPart=u.dwHighDateTime;
                    base[n].pid = pe.th32ProcessID;
                    base[n].cpu100ns = ku.QuadPart + uu.QuadPart;
                    ++n;
                }
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static BOOL find_baseline(const ProcBase *base, int n, DWORD pid, ULONGLONG *v) {
    for (int i = 0; i < n; ++i) {
        if (base[i].pid == pid) { *v = base[i].cpu100ns; return TRUE; }
    }
    return FALSE;
}

static void write_process_snapshot(const WCHAR *dir) {
    ProcBase base[MAX_PROCS];
    int bn = collect_proc_baseline(base, MAX_PROCS);
    const DWORD window_ms = 250;
    Sleep(window_ms);

    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    ProcRow *rows = (ProcRow*)calloc(MAX_PROCS, sizeof(ProcRow));
    if (!rows) return;
    int rn = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; ZeroMemory(&pe, sizeof(pe)); pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (rn >= MAX_PROCS) break;
                ProcRow *r = &rows[rn];
                r->pid = pe.th32ProcessID; r->ppid = pe.th32ParentProcessID;
                _snwprintf(r->path, _countof(r->path), L"%ls", pe.szExeFile);
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, r->pid);
                if (h) {
                    WCHAR full[MAX_PATH_W]; DWORD cc = _countof(full);
                    if (QueryFullProcessImageNameW(h, 0, full, &cc)) _snwprintf(r->path, _countof(r->path), L"%ls", full);
                    FILETIME c,e,k,u;
                    if (GetProcessTimes(h, &c, &e, &k, &u)) {
                        ULARGE_INTEGER ku, uu; ku.LowPart=k.dwLowDateTime; ku.HighPart=k.dwHighDateTime; uu.LowPart=u.dwLowDateTime; uu.HighPart=u.dwHighDateTime;
                        ULONGLONG now = ku.QuadPart + uu.QuadPart, old = 0;
                        if (find_baseline(base, bn, r->pid, &old) && now >= old) {
                            double one = (double)(now - old) / ((double)window_ms * 10000.0) * 100.0;
                            r->cpu_one_core_pct = one;
                            r->cpu_system_pct = one / (double)cores;
                        }
                    }
                    PROCESS_MEMORY_COUNTERS_EX pmc; ZeroMemory(&pmc, sizeof(pmc)); pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                        r->working_set = pmc.WorkingSetSize; r->private_bytes = pmc.PrivateUsage;
                    }
                    IO_COUNTERS io; if (GetProcessIoCounters(h, &io)) { r->read_bytes = io.ReadTransferCount; r->write_bytes = io.WriteTransferCount; }
                    GetProcessHandleCount(h, &r->handles);
                    CloseHandle(h);
                }
                ++rn;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    qsort(rows, (size_t)rn, sizeof(ProcRow), cmp_proc_cpu);

    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\processes.csv", dir);
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        fputs("pid,ppid,cpu_system_pct,cpu_one_core_pct,working_set_MB,private_MB,read_MB_total,write_MB_total,handles,path\r\n", f);
        for (int i = 0; i < rn; ++i) {
            ProcRow *r = &rows[i];
            fprintf(f, "%lu,%lu,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%lu,",
                (unsigned long)r->pid, (unsigned long)r->ppid,
                r->cpu_system_pct, r->cpu_one_core_pct,
                (double)r->working_set / 1048576.0,
                (double)r->private_bytes / 1048576.0,
                (double)r->read_bytes / 1048576.0,
                (double)r->write_bytes / 1048576.0,
                (unsigned long)r->handles);
            csv_w(f, r->path); fputs("\r\n", f);
        }
        fclose(f);
    }
    free(rows);
}

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

static BOOL write_wait_chains(const WCHAR *dir, DWORD target_pid) {
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\waitchains.txt", dir);
    FILE *f = _wfopen(path, L"wb");
    if (!f) return FALSE;
    fprintf(f, "Target PID: %lu\r\n", (unsigned long)target_pid);
    if (!target_pid) { fputs("No foreground target process.\r\n", f); fclose(f); return FALSE; }

    HWCT session = OpenThreadWaitChainSession(0, NULL);
    if (!session) { fprintf(f, "OpenThreadWaitChainSession failed: %lu\r\n", GetLastError()); fclose(f); return FALSE; }

    BOOL any_cycle = FALSE;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; ZeroMemory(&te, sizeof(te)); te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != target_pid) continue;
                WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
                DWORD count = WCT_MAX_NODE_COUNT; BOOL cycle = FALSE;
                ZeroMemory(nodes, sizeof(nodes));
                if (GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, te.th32ThreadID, &count, nodes, &cycle)) {
                    fprintf(f, "\r\nThread %lu  cycle=%s  nodes=%lu\r\n", (unsigned long)te.th32ThreadID, cycle ? "YES" : "no", (unsigned long)count);
                    if (cycle) any_cycle = TRUE;
                    for (DWORD i = 0; i < count; ++i) {
                        WAITCHAIN_NODE_INFO *n = &nodes[i];
                        fprintf(f, "  [%lu] %-16s status=%d", (unsigned long)i, wct_type_name(n->ObjectType), (int)n->ObjectStatus);
                        if (n->ObjectType == WctThreadType) {
                            fprintf(f, " pid=%lu tid=%lu wait_ms=%lu switches=%lu",
                                (unsigned long)n->ThreadObject.ProcessId,
                                (unsigned long)n->ThreadObject.ThreadId,
                                (unsigned long)n->ThreadObject.WaitTime,
                                (unsigned long)n->ThreadObject.ContextSwitches);
                        } else if (n->LockObject.ObjectName[0]) {
                            fputs(" name=", f); write_utf8(f, n->LockObject.ObjectName);
                        }
                        fputs("\r\n", f);
                    }
                } else {
                    fprintf(f, "Thread %lu: GetThreadWaitChain failed: %lu\r\n", (unsigned long)te.th32ThreadID, GetLastError());
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    CloseThreadWaitChainSession(session);
    fclose(f);
    return any_cycle;
}

static void write_threads(const WCHAR *dir, DWORD target_pid) {
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\threads.csv", dir);
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fputs("tid,base_priority,current_priority,kernel_ms,user_ms,created_utc_filetime\r\n", f);
    if (!target_pid) { fclose(f); return; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; ZeroMemory(&te, sizeof(te)); te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != target_pid) continue;
                double kms = -1.0, ums = -1.0; ULONGLONG created = 0; int pri = THREAD_PRIORITY_ERROR_RETURN;
                HANDLE ht = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (ht) {
                    FILETIME c,e,k,u;
                    if (GetThreadTimes(ht, &c, &e, &k, &u)) {
                        ULARGE_INTEGER ci,ki,ui;
                        ci.LowPart=c.dwLowDateTime; ci.HighPart=c.dwHighDateTime;
                        ki.LowPart=k.dwLowDateTime; ki.HighPart=k.dwHighDateTime;
                        ui.LowPart=u.dwLowDateTime; ui.HighPart=u.dwHighDateTime;
                        created = ci.QuadPart; kms = (double)ki.QuadPart / 10000.0; ums = (double)ui.QuadPart / 10000.0;
                    }
                    pri = GetThreadPriority(ht);
                    CloseHandle(ht);
                }
                fprintf(f, "%lu,%ld,%d,%.3f,%.3f,%llu\r\n",
                    (unsigned long)te.th32ThreadID, (long)te.tpBasePri, pri, kms, ums, (unsigned long long)created);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    fclose(f);
}

static void write_modules(const WCHAR *dir, DWORD target_pid) {
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\modules.csv", dir);
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fputs("base,size,module,path\r\n", f);
    if (!target_pid) { fclose(f); return; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, target_pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me; ZeroMemory(&me, sizeof(me)); me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                fprintf(f, "0x%p,%lu,", me.modBaseAddr, (unsigned long)me.modBaseSize);
                csv_w(f, me.szModule); fputc(',', f); csv_w(f, me.szExePath); fputs("\r\n", f);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    fclose(f);
}

static BOOL write_minidump(const WCHAR *dir, DWORD pid) {
    if (!pid) return FALSE;
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!hp) hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return FALSE;
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\target.dmp", dir);
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { CloseHandle(hp); return FALSE; }
    MINIDUMP_TYPE mt = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    BOOL ok = MiniDumpWriteDump(hp, pid, hf, mt, NULL, NULL, NULL);
    CloseHandle(hf); CloseHandle(hp);
    if (!ok) DeleteFileW(path);
    return ok;
}

static DWORD scan_event_flags(const WCHAR *xml) {
    DWORD f = 0;
    if (wcsstr(xml, L"WHEA") || wcsstr(xml, L"HardwareError")) f |= EVENTFLAG_WHEA;
    if (wcsstr(xml, L"storport") || wcsstr(xml, L"storahci") || wcsstr(xml, L"stornvme") || wcsstr(xml, L"<Provider Name='disk'") || wcsstr(xml, L"<Provider Name=\"disk\"")) f |= EVENTFLAG_STORAGE;
    if (wcsstr(xml, L"Display") || wcsstr(xml, L"nvlddmkm") || wcsstr(xml, L"amdkmdag") || wcsstr(xml, L"igfx")) f |= EVENTFLAG_DISPLAY;
    if (wcsstr(xml, L"Application Hang") || wcsstr(xml, L"AppHang")) f |= EVENTFLAG_APPHANG;
    if (wcsstr(xml, L"Kernel-Power") || wcsstr(xml, L"Power-Troubleshooter")) f |= EVENTFLAG_POWER;
    if (wcsstr(xml, L"Ntfs") || wcsstr(xml, L"volmgr")) f |= EVENTFLAG_NTFS;
    return f;
}

static DWORD export_events_channel(const WCHAR *dir, const WCHAR *channel, const WCHAR *filename, DWORD lookback_ms) {
    WCHAR query[512];
    _snwprintf(query, _countof(query), L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= %lu]]]", (unsigned long)lookback_ms);
    EVT_HANDLE q = EvtQuery(NULL, channel, query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) return 0;
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\%ls", dir, filename);
    FILE *f = _wfopen(path, L"wb");
    if (!f) { EvtClose(q); return 0; }
    fputs("<!-- Recent Warning/Error/Critical events, newest first -->\r\n<Events>\r\n", f);
    DWORD flags = 0, total = 0;
    EVT_HANDLE ev[16]; DWORD got = 0;
    while (total < 160 && EvtNext(q, 16, ev, 0, 0, &got)) {
        for (DWORD i = 0; i < got && total < 160; ++i, ++total) {
            DWORD need = 0, used = 0, props = 0;
            EvtRender(NULL, ev[i], EvtRenderEventXml, 0, NULL, &need, &props);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && need < MAX_EVENT_XML * sizeof(WCHAR)) {
                WCHAR *xml = (WCHAR*)malloc(need);
                if (xml && EvtRender(NULL, ev[i], EvtRenderEventXml, need, xml, &used, &props)) {
                    flags |= scan_event_flags(xml);
                    write_utf8(f, xml); fputs("\r\n", f);
                }
                free(xml);
            }
            EvtClose(ev[i]);
        }
    }
    fputs("</Events>\r\n", f);
    fclose(f); EvtClose(q);
    return flags;
}

static void write_drives(const WCHAR *dir) {
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\drives.csv", dir);
    FILE *f = _wfopen(path, L"wb"); if (!f) return;
    fputs("drive,type,total_GB,free_GB,free_pct\r\n", f);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        WCHAR root[4] = {(WCHAR)(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        ULARGE_INTEGER freeAvail, total, freeTotal;
        if (GetDiskFreeSpaceExW(root, &freeAvail, &total, &freeTotal)) {
            double pct = total.QuadPart ? (double)freeTotal.QuadPart * 100.0 / (double)total.QuadPart : 0.0;
            fprintf(f, "%c,%u,%.2f,%.2f,%.2f\r\n", 'A'+i, type,
                (double)total.QuadPart / 1073741824.0,
                (double)freeTotal.QuadPart / 1073741824.0, pct);
        }
    }
    fclose(f);
}

static void max_recent_metrics(const Sample *ring, int count, int head, Metrics *maxm, int *max_late) {
    ZeroMemory(maxm, sizeof(*maxm)); *max_late = 0;
    int start = (head - count + MAX_RING) % MAX_RING;
    for (int i = 0; i < count; ++i) {
        const Sample *s = &ring[(start + i) % MAX_RING];
        if (s->wake_late_ms > *max_late) *max_late = s->wake_late_ms;
        if (s->m.cpu > maxm->cpu) maxm->cpu = s->m.cpu;
        if (s->m.dpc > maxm->dpc) maxm->dpc = s->m.dpc;
        if (s->m.interrupt > maxm->interrupt) maxm->interrupt = s->m.interrupt;
        if (s->m.disk_queue > maxm->disk_queue) maxm->disk_queue = s->m.disk_queue;
        if (s->m.commit_pct > maxm->commit_pct) maxm->commit_pct = s->m.commit_pct;
        if (s->m.proc_queue > maxm->proc_queue) maxm->proc_queue = s->m.proc_queue;
        if (s->m.mem_load > maxm->mem_load) maxm->mem_load = s->m.mem_load;
    }
}

static void write_summary(const WCHAR *dir, const Config *cfg, TriggerReason reason, const Sample *trigger_sample,
                          const Sample *ring, int ring_count, int ring_head, DWORD event_flags, BOOL wait_cycle, BOOL dump_ok) {
    WCHAR path[MAX_PATH_W]; _snwprintf(path, _countof(path), L"%ls\\summary.txt", dir);
    FILE *f = _wfopen(path, L"wb"); if (!f) return;
    Metrics mx; int max_late = 0; max_recent_metrics(ring, ring_count, ring_head, &mx, &max_late);
    SYSTEM_INFO si; GetSystemInfo(&si);

    fputs("WinFreezeTrace incident report\r\n==============================\r\n", f);
    fprintf(f, "Trigger: %s\r\n", trigger_name(reason));
    fprintf(f, "Time: %04u-%02u-%02u %02u:%02u:%02u.%03u local\r\n",
        trigger_sample->ts.wYear, trigger_sample->ts.wMonth, trigger_sample->ts.wDay,
        trigger_sample->ts.wHour, trigger_sample->ts.wMinute, trigger_sample->ts.wSecond, trigger_sample->ts.wMilliseconds);
    fprintf(f, "Wake lateness at trigger: %d ms (threshold %lu ms)\r\n", trigger_sample->wake_late_ms, (unsigned long)cfg->stall_ms);
    fprintf(f, "Foreground PID/TID: %lu / %lu\r\nForeground EXE: ", (unsigned long)trigger_sample->fg_pid, (unsigned long)trigger_sample->fg_tid); write_utf8(f, trigger_sample->fg_path);
    fputs("\r\nWindow class: ", f); write_utf8(f, trigger_sample->fg_class);
    fputs("\r\nForeground title: ", f); write_utf8(f, trigger_sample->fg_title); fputs("\r\n", f);
    fprintf(f, "Foreground responsiveness test: %s\r\n", trigger_sample->fg_hung ? "TIMEOUT/HUNG" : "responded");
    fprintf(f, "Logical CPU: %lu\r\n", (unsigned long)si.dwNumberOfProcessors);
    fprintf(f, "Recent maxima: CPU %.1f%% | DPC %.1f%% | Interrupt %.1f%% | DiskQ %.2f | Commit %.1f%% | ProcQ %.1f | WakeLate %d ms\r\n",
        mx.cpu, mx.dpc, mx.interrupt, mx.disk_queue, mx.commit_pct, mx.proc_queue, max_late);
    fprintf(f, "Wait-chain cycle: %s\r\nMinidump: %s\r\n", wait_cycle ? "YES" : "no", dump_ok ? "written" : (cfg->make_dump ? "not available" : "disabled"));

    fputs("\r\nLikely-cause analysis\r\n---------------------\r\n", f);
    int findings = 0;
    if (wait_cycle) { fputs("[HIGH] A wait-chain cycle was found in the foreground process. This is strong evidence of a deadlock/circular wait. See waitchains.txt and target.dmp.\r\n", f); ++findings; }
    if (event_flags & EVENTFLAG_WHEA) { fputs("[HIGH] Recent WHEA/hardware-error events exist. Check CPU/RAM/PCIe/GPU/storage hardware and firmware. See events_system.xml.\r\n", f); ++findings; }
    if (event_flags & EVENTFLAG_STORAGE) { fputs("[HIGH/MED] Recent storage driver/disk events exist (storport/storahci/stornvme/disk). This can explain whole-system stalls. See events_system.xml.\r\n", f); ++findings; }
    if (event_flags & EVENTFLAG_DISPLAY) { fputs("[MED] Recent display/GPU-driver events exist. A GPU driver timeout/reset may look like a freeze. See events_system.xml.\r\n", f); ++findings; }
    if (event_flags & EVENTFLAG_APPHANG) { fputs("[MED] Windows logged an Application Hang near this incident. Correlate the app name/time with the foreground target.\r\n", f); ++findings; }
    if (mx.commit_pct >= 95.0 || mx.mem_load >= 97) { fputs("[MED] Memory/commit pressure was extreme before the incident. Paging or allocation stalls are plausible.\r\n", f); ++findings; }
    if (mx.dpc >= 20.0 || mx.interrupt >= 20.0) { fputs("[MED] DPC/interrupt CPU time spiked. This points toward a driver/device interrupt-latency problem.\r\n", f); ++findings; }
    if (mx.cpu >= 97.0 && mx.proc_queue > (double)si.dwNumberOfProcessors) { fputs("[MED] CPU saturation plus a long processor queue was observed. Scheduler starvation is plausible; check top entries in processes.csv.\r\n", f); ++findings; }
    if (mx.disk_queue >= 4.0 && mx.cpu < 90.0) { fputs("[MED] Disk queue was elevated without matching CPU saturation. Storage latency/queueing is plausible; correlate with storage events.\r\n", f); ++findings; }
    if (reason == TRIGGER_APP_HANG && max_late < (int)cfg->stall_ms) { fputs("[MED] The foreground app stopped responding while the watchdog itself kept scheduling normally. This favors an application-specific wait/deadlock over a whole-system freeze.\r\n", f); ++findings; }
    if (reason == TRIGGER_SCHEDULER_STALL && findings == 0) { fputs("[LOW] The watchdog was delayed but no strong CPU/memory/disk/event-log signature was captured. Suspect a low-level driver, firmware, GPU, storage, or transient scheduler stall. target.dmp helps only for the foreground app; a kernel ETW trace may be needed if this repeats.\r\n", f); ++findings; }
    if (!findings) fputs("[LOW] No single cause is strong enough from this capture. Use timeline.csv + processes.csv + waitchains.txt + Event Log correlation.\r\n", f);

    fputs("\r\nFiles\r\n-----\r\n", f);
    fputs("timeline.csv        Rolling pre-trigger history kept in RAM (no continuous disk writes).\r\n", f);
    fputs("processes.csv       Trigger-time process CPU/memory/I/O snapshot, sorted by CPU.\r\n", f);
    fputs("waitchains.txt      Wait Chain Traversal for all threads in the foreground process.\r\n", f);
    fputs("threads.csv         Foreground-process thread IDs, priorities, and accumulated CPU time.\r\n", f);
    fputs("modules.csv         DLL/module map for the foreground process.\r\n", f);
    fputs("target.dmp          Small thread-focused minidump, when access allows.\r\n", f);
    fputs("events_system.xml   Recent System critical/error/warning events.\r\n", f);
    fputs("events_application.xml Recent Application critical/error/warning events.\r\n", f);
    fputs("drives.csv          Free-space snapshot to spot critically full volumes.\r\n", f);
    fclose(f);
}

static void deep_capture(const Config *cfg, TriggerReason reason, const Sample *trigger_sample,
                         const Sample *ring, int ring_count, int ring_head) {
    WCHAR dir[MAX_PATH_W]; make_report_dir(cfg, &trigger_sample->ts, reason, dir, _countof(dir));
    write_timeline(dir, ring, ring_count, ring_head);
    write_process_snapshot(dir);
    BOOL cycle = write_wait_chains(dir, trigger_sample->fg_pid);
    write_threads(dir, trigger_sample->fg_pid);
    write_modules(dir, trigger_sample->fg_pid);
    BOOL dump_ok = FALSE;
    if (cfg->make_dump) dump_ok = write_minidump(dir, trigger_sample->fg_pid);
    DWORD flags = 0;
    flags |= export_events_channel(dir, L"System", L"events_system.xml", 5 * 60 * 1000);
    flags |= export_events_channel(dir, L"Application", L"events_application.xml", 5 * 60 * 1000);
    write_drives(dir);
    write_summary(dir, cfg, reason, trigger_sample, ring, ring_count, ring_head, flags, cycle, dump_ok);
}

static void parse_args(Config *cfg) {
    cfg->interval_ms = 1000;
    cfg->history_seconds = 180;
    cfg->stall_ms = 1500;
    cfg->hang_timeout_ms = 250;
    cfg->hang_samples = 3;
    cfg->cooldown_seconds = 60;
    cfg->make_dump = TRUE;
    cfg->background = TRUE;
    default_report_root(cfg->report_root, _countof(cfg->report_root));

    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--no-dump")) cfg->make_dump = FALSE;
        else if (!_wcsicmp(argv[i], L"--background")) cfg->background = TRUE;
        else if (!_wcsicmp(argv[i], L"--interval") && i + 1 < argc) cfg->interval_ms = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--history") && i + 1 < argc) cfg->history_seconds = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--stall-ms") && i + 1 < argc) cfg->stall_ms = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--hang-timeout-ms") && i + 1 < argc) cfg->hang_timeout_ms = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--hang-samples") && i + 1 < argc) cfg->hang_samples = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--cooldown") && i + 1 < argc) cfg->cooldown_seconds = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"--report-dir") && i + 1 < argc) _snwprintf(cfg->report_root, _countof(cfg->report_root), L"%ls", argv[++i]);
    }
    LocalFree(argv);
    if (cfg->interval_ms < 250) cfg->interval_ms = 250;
    if (cfg->history_seconds < 30) cfg->history_seconds = 30;
    if (cfg->history_seconds > 600) cfg->history_seconds = 600;
    if (cfg->stall_ms < 500) cfg->stall_ms = 500;
    if (cfg->hang_samples < 2) cfg->hang_samples = 2;
    ensure_dir(cfg->report_root);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    (void)hInst; (void)hPrev; (void)cmd; (void)show;
    HANDLE singleton = CreateMutexW(NULL, TRUE, L"Local\\WinFreezeTrace.Singleton.v2");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) return 2;

    QueryPerformanceFrequency(&g_qpf);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    Config cfg; parse_args(&cfg);
    PdhState pdh; pdh_init(&pdh);
    HANDLE hk = CreateThread(NULL, 0, hotkey_thread, NULL, 0, NULL);
    if (hk) { SetThreadPriority(hk, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(hk); }

    Sample ring[MAX_RING]; ZeroMemory(ring, sizeof(ring));
    int ring_head = 0, ring_count = 0;
    int max_samples = (int)(cfg.history_seconds * 1000 / cfg.interval_ms);
    if (max_samples < 1) max_samples = 1;
    if (max_samples > MAX_RING) max_samples = MAX_RING;
    DWORD hung_streak = 0;
    double last_trigger_ms = -1e12;

    for (;;) {
        double before_sleep = qpc_ms();
        Sleep(cfg.interval_ms);
        double wake = qpc_ms();
        int late = (int)((wake - before_sleep) - (double)cfg.interval_ms);
        if (late < 0) late = 0;

        Sample s; ZeroMemory(&s, sizeof(s));
        GetLocalTime(&s.ts);
        s.wake_late_ms = late;
        collect_metrics(&pdh, &s.m);
        get_foreground(&s.fg_pid, &s.fg_tid, s.fg_title, _countof(s.fg_title), s.fg_class, _countof(s.fg_class), s.fg_path, _countof(s.fg_path), &s.fg_hung, cfg.hang_timeout_ms);

        if (s.fg_hung) ++hung_streak; else hung_streak = 0;
        ring[ring_head] = s;
        ring_head = (ring_head + 1) % MAX_RING;
        if (ring_count < max_samples) ++ring_count;

        TriggerReason reason = 0;
        if (InterlockedExchange(&g_manual_trigger, 0)) reason = TRIGGER_MANUAL;
        else if ((DWORD)late >= cfg.stall_ms && (DWORD)late < 60000) reason = TRIGGER_SCHEDULER_STALL;
        else if (hung_streak >= cfg.hang_samples) reason = TRIGGER_APP_HANG;

        double now = qpc_ms();
        if (reason && (now - last_trigger_ms) >= (double)cfg.cooldown_seconds * 1000.0) {
            deep_capture(&cfg, reason, &s, ring, ring_count, ring_head);
            last_trigger_ms = qpc_ms();
            hung_streak = 0;
        }
    }

    pdh_close(&pdh);
    ReleaseMutex(singleton); CloseHandle(singleton);
    return 0;
}
