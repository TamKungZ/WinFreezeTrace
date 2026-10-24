#include "procsnap.h"

#define MAX_PROCS 16384

static int cmp_base(const void *a, const void *b) {
    DWORD x = ((const ProcBase *)a)->pid, y = ((const ProcBase *)b)->pid;
    return (x > y) - (x < y);
}
static int cmp_cpu(const void *a, const void *b) {
    double x = ((const ProcRow *)a)->cpu_system_pct, y = ((const ProcRow *)b)->cpu_system_pct;
    return (x < y) - (x > y);
}
static ULONGLONG ft64(FILETIME f) { ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime; return u.QuadPart; }

void procsnap_begin(ProcSnap *ps) {
    memset(ps, 0, sizeof *ps);
    ps->base = (ProcBase *)calloc(MAX_PROCS, sizeof(ProcBase));
    ps->base_ms = plat_qpc_ms();
    if (!ps->base) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; memset(&pe, 0, sizeof pe); pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (ps->base_n >= MAX_PROCS) { ps->truncated = true; break; }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            FILETIME c, e, k, u; IO_COUNTERS io; memset(&io, 0, sizeof io);
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                ProcBase *b = &ps->base[ps->base_n++];
                b->pid = pe.th32ProcessID; b->created = ft64(c); b->cpu100ns = ft64(k) + ft64(u);
                if (GetProcessIoCounters(h, &io)) { b->read_bytes = io.ReadTransferCount; b->write_bytes = io.WriteTransferCount; }
                PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof pmc;
                if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) b->faults = pmc.PageFaultCount;
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    qsort(ps->base, (size_t)ps->base_n, sizeof(ProcBase), cmp_base);
}

void procsnap_finish(ProcSnap *ps, DWORD fg_pid) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    double cores = si.dwNumberOfProcessors ? (double)si.dwNumberOfProcessors : 1.0;
    double window_ms = plat_qpc_ms() - ps->base_ms;
    if (window_ms < 1.0) window_ms = 1.0;
    ps->base_ms = window_ms;      /* from here on (also on the early-return paths): the measured window length */
    int cap = 512; ps->rows = (ProcRow *)calloc((size_t)cap, sizeof(ProcRow));
    if (!ps->rows) return;
    FILETIME nowft; GetSystemTimeAsFileTime(&nowft); ULONGLONG now100 = ft64(nowft);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; memset(&pe, 0, sizeof pe); pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (ps->row_n >= MAX_PROCS) { ps->truncated = true; break; }
            if (ps->row_n == cap) {
                ProcRow *nr = (ProcRow *)realloc(ps->rows, (size_t)cap * 2 * sizeof(ProcRow));
                if (!nr) break;
                memset(nr + cap, 0, (size_t)cap * sizeof(ProcRow));
                ps->rows = nr; cap *= 2;
            }
            ProcRow *r = &ps->rows[ps->row_n++];
            r->pid = pe.th32ProcessID; r->ppid = pe.th32ParentProcessID; r->threads = pe.cntThreads;
            r->is_fg = (pe.th32ProcessID == fg_pid);
            wft_wsnprintf(r->path, sizeof r->path / sizeof r->path[0], L"%ls", pe.szExeFile);
            ProcessIdToSessionId(r->pid, &r->session);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r->pid);   /* no PROCESS_VM_READ needed */
            if (!h) { r->no_access = true; continue; }
            WCHAR full[WFT_PATH_CCH]; DWORD cc = WFT_PATH_CCH;
            if (QueryFullProcessImageNameW(h, 0, full, &cc)) wft_wsnprintf(r->path, sizeof r->path / sizeof r->path[0], L"%ls", full);
            ProcBase key; key.pid = r->pid;
            ProcBase *b = (ProcBase *)bsearch(&key, ps->base, (size_t)ps->base_n, sizeof(ProcBase), cmp_base);
            FILETIME c = {0, 0}, e, k, u;
            bool have_times = GetProcessTimes(h, &c, &e, &k, &u) != 0;
            bool same = have_times && b && b->created == ft64(c);            /* same process, not a recycled PID */
            if (have_times) {
                ULONGLONG cur = ft64(k) + ft64(u);
                r->cpu_total_s = (double)cur / 1e7;
                r->age_min = now100 > ft64(c) ? (double)(now100 - ft64(c)) / 6e8 : 0.0;
                if (same && cur >= b->cpu100ns) {
                    r->cpu_one_core_pct = (double)(cur - b->cpu100ns) / (window_ms * 10000.0) * 100.0;
                    r->cpu_system_pct = r->cpu_one_core_pct / cores;
                }
            }
            PROCESS_MEMORY_COUNTERS_EX pmc; memset(&pmc, 0, sizeof pmc); pmc.cb = sizeof pmc;
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
                r->working_set = pmc.WorkingSetSize; r->peak_working_set = pmc.PeakWorkingSetSize;
                r->private_bytes = pmc.PrivateUsage; r->page_faults = pmc.PageFaultCount;
                if (same && pmc.PageFaultCount >= b->faults) r->faults_per_s = (double)(pmc.PageFaultCount - b->faults) / (window_ms / 1000.0);
            }
            IO_COUNTERS io;
            if (GetProcessIoCounters(h, &io)) {
                r->read_bytes = io.ReadTransferCount; r->write_bytes = io.WriteTransferCount;
                r->read_ops = io.ReadOperationCount; r->write_ops = io.WriteOperationCount;
                if (same && io.ReadTransferCount >= b->read_bytes && io.WriteTransferCount >= b->write_bytes) {
                    r->read_bps = (double)(io.ReadTransferCount - b->read_bytes) / (window_ms / 1000.0);
                    r->write_bps = (double)(io.WriteTransferCount - b->write_bytes) / (window_ms / 1000.0);
                }
            }
            GetProcessHandleCount(h, &r->handles);
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    qsort(ps->rows, (size_t)ps->row_n, sizeof(ProcRow), cmp_cpu);
}

bool procsnap_write_csv(const WCHAR *dir, const ProcSnap *ps, const Config *cfg) {
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\processes.csv", dir);
    FILE *f = plat_fopen_w(path); if (!f) return false;
    fputs("pid,ppid,session,is_foreground,threads,cpu_system_pct,cpu_one_core_pct,cpu_total_s,age_min,working_set_MB,peak_working_set_MB,private_MB,"
          "page_faults_total,page_faults_per_s,read_Bps,write_Bps,read_ops_total,write_ops_total,read_MB_total,write_MB_total,handles,no_access,path\r\n", f);
    for (int i = 0; i < ps->row_n; ++i) {
        const ProcRow *r = &ps->rows[i];
        fprintf(f, "%lu,%lu,%lu,%d,%lu,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%.0f,%.0f,%.0f,%llu,%llu,%.1f,%.1f,%lu,%d,",
            (unsigned long)r->pid, (unsigned long)r->ppid, (unsigned long)r->session, r->is_fg ? 1 : 0, (unsigned long)r->threads,
            r->cpu_system_pct, r->cpu_one_core_pct, r->cpu_total_s, r->age_min,
            (double)r->working_set / 1048576.0, (double)r->peak_working_set / 1048576.0, (double)r->private_bytes / 1048576.0,
            (unsigned long)r->page_faults, r->faults_per_s, r->read_bps, r->write_bps,
            (unsigned long long)r->read_ops, (unsigned long long)r->write_ops,
            (double)r->read_bytes / 1048576.0, (double)r->write_bytes / 1048576.0, (unsigned long)r->handles, r->no_access ? 1 : 0);
        WCHAR p[520]; wft_wsnprintf(p, 520, L"%ls", r->path);
        if (cfg->redact) wft_redact_user_path(p, 520);
        wft_csv_text(f, p); fputs("\r\n", f);
    }
    bool ok = ferror(f) == 0;
    return fclose(f) == 0 && ok;
}

int procsnap_top(const ProcSnap *ps, int kind, TopProc *out, int max_out) {
    int n = 0;
    for (int i = 0; i < ps->row_n; ++i) {
        const ProcRow *r = &ps->rows[i];
        double v = kind == 0 ? r->cpu_system_pct : r->read_bps + r->write_bps;
        if (v <= 0.0 || r->pid == 0) continue;
        int pos = 0;
        while (pos < n && out[pos].value >= v) ++pos;
        if (pos >= max_out) continue;
        for (int j = (n < max_out ? n : max_out - 1); j > pos; --j) out[j] = out[j - 1];
        out[pos].pid = r->pid; out[pos].value = v;
        wft_ascii_from_wide(out[pos].name, sizeof out[pos].name, wft_base_name(r->path));
        if (n < max_out) ++n;
    }
    return n;
}

void procsnap_free(ProcSnap *ps) { free(ps->base); free(ps->rows); memset(ps, 0, sizeof *ps); }
