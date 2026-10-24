#include "metrics.h"
#include "stats.h"

typedef struct SingleDef { const WCHAR *path; size_t off; double scale; } SingleDef;
typedef struct ArrDef    { const WCHAR *path; size_t off_val; size_t off_name; size_t name_cch; double scale; } ArrDef;
#define NONAME ((size_t)-1)

static const SingleDef k_single[WFT_PDH_SINGLES] = {
    { L"\\Processor(_Total)\\% Processor Time",              offsetof(Metrics, cpu), 1.0 },
    { L"\\Processor(_Total)\\% User Time",                   offsetof(Metrics, cpu_user), 1.0 },
    { L"\\Processor(_Total)\\% Privileged Time",             offsetof(Metrics, cpu_privileged), 1.0 },
    { L"\\Processor(_Total)\\% DPC Time",                    offsetof(Metrics, dpc), 1.0 },
    { L"\\Processor(_Total)\\% Interrupt Time",              offsetof(Metrics, interrupt), 1.0 },
    { L"\\Processor(_Total)\\Interrupts/sec",                offsetof(Metrics, interrupts_per_sec), 1.0 },
    { L"\\Processor(_Total)\\DPCs Queued/sec",               offsetof(Metrics, dpcs_queued_per_sec), 1.0 },
    { L"\\System\\Context Switches/sec",                     offsetof(Metrics, context_switches_per_sec), 1.0 },
    { L"\\System\\Processor Queue Length",                   offsetof(Metrics, proc_queue), 1.0 },
    { L"\\PhysicalDisk(_Total)\\Avg. Disk Queue Length",     offsetof(Metrics, disk_queue), 1.0 },
    { L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",        offsetof(Metrics, disk_read_bps), 1.0 },
    { L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",       offsetof(Metrics, disk_write_bps), 1.0 },
    { L"\\PhysicalDisk(_Total)\\Disk Transfers/sec",         offsetof(Metrics, disk_iops), 1.0 },
    { L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Transfer",     offsetof(Metrics, disk_latency_ms), 1000.0 },
    { L"\\Memory\\% Committed Bytes In Use",                 offsetof(Metrics, commit_pct), 1.0 },
    { L"\\Memory\\Pages Input/sec",                          offsetof(Metrics, pages_input_per_sec), 1.0 },
    { L"\\Memory\\Page Reads/sec",                           offsetof(Metrics, page_reads_per_sec), 1.0 },
    { L"\\Memory\\Page Faults/sec",                          offsetof(Metrics, page_faults_per_sec), 1.0 },
    { L"\\Processor Information(_Total)\\% Processor Performance", offsetof(Metrics, cpu_perf_pct), 1.0 },
    { L"\\Processor Information(_Total)\\Processor Frequency",     offsetof(Metrics, cpu_mhz), 1.0 },
};

static const ArrDef k_arr[WFT_PDH_ARRAYS] = {
    { L"\\Processor Information(*)\\% Processor Time",  offsetof(Metrics, core_cpu_max),       offsetof(Metrics, core_cpu_name),   16, 1.0 },
    { L"\\Processor Information(*)\\% DPC Time",        offsetof(Metrics, core_dpc_max),       offsetof(Metrics, core_dpc_name),   16, 1.0 },
    { L"\\Processor Information(*)\\% Interrupt Time",  offsetof(Metrics, core_interrupt_max), NONAME,                              0, 1.0 },
    { L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer",     offsetof(Metrics, disk_worst_latency_ms), offsetof(Metrics, disk_worst_name), 32, 1000.0 },
    { L"\\PhysicalDisk(*)\\Avg. Disk Queue Length",     offsetof(Metrics, disk_worst_queue),   NONAME,                              0, 1.0 },
};

static void add_counter(PDH_HQUERY q, const WCHAR *path, PDH_HCOUNTER *out, int *missing) {
    *out = NULL;
    if (PdhAddEnglishCounterW(q, path, 0, out) != ERROR_SUCCESS) {   /* locale-independent first */
        *out = NULL;
        if (PdhAddCounterW(q, path, 0, out) != ERROR_SUCCESS) { *out = NULL; ++*missing; }
    }
}

static double value_of(PDH_HCOUNTER c) {
    if (!c) return WFT_NA;
    PDH_FMT_COUNTERVALUE v; DWORD type = 0;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, &type, &v) != ERROR_SUCCESS) return WFT_NA;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return WFT_NA;
    return v.doubleValue < 0.0 ? 0.0 : v.doubleValue;       /* WFT_NA is negative; real counters never are */
}

/* Reads every instance of a wildcard counter; reports the maximum (or minimum) excluding _Total instances. */
static bool read_array(ArrayCounter *a, double scale, bool want_max, double *out, char *name, size_t name_cch) {
    if (!a->h) return false;
    DWORD bytes = a->cap, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(a->h, PDH_FMT_DOUBLE, &bytes, &count, (PDH_FMT_COUNTERVALUE_ITEM_W *)a->buf);
    if (st == (PDH_STATUS)PDH_MORE_DATA && bytes > a->cap && bytes <= (1u << 20)) {
        BYTE *nb = (BYTE *)realloc(a->buf, bytes);
        if (!nb) return false;
        a->buf = nb; a->cap = bytes;
        st = PdhGetFormattedCounterArrayW(a->h, PDH_FMT_DOUBLE, &bytes, &count, (PDH_FMT_COUNTERVALUE_ITEM_W *)a->buf);
    }
    if (st != ERROR_SUCCESS) return false;
    const PDH_FMT_COUNTERVALUE_ITEM_W *it = (const PDH_FMT_COUNTERVALUE_ITEM_W *)a->buf;
    bool found = false; double best = 0;
    for (DWORD i = 0; i < count; ++i) {
        if (!it[i].szName || wcsstr(it[i].szName, L"_Total")) continue;
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        double v = it[i].FmtValue.doubleValue * scale;
        if (v < 0.0) v = 0.0;
        if (!found || (want_max ? v > best : v < best)) {
            found = true; best = v;
            if (name && name_cch) wft_ascii_from_wide(name, name_cch, it[i].szName);
        }
    }
    if (found) *out = best;
    return found;
}

bool metrics_init(PdhState *p) {
    memset(p, 0, sizeof *p);
    if (PdhOpenQueryW(NULL, 0, &p->q) != ERROR_SUCCESS) return false;
    for (int i = 0; i < WFT_PDH_SINGLES; ++i) if (k_single[i].path) add_counter(p->q, k_single[i].path, &p->single[i], &p->missing);
    for (int i = 0; i < WFT_PDH_ARRAYS; ++i)  if (k_arr[i].path) add_counter(p->q, k_arr[i].path, &p->arr[i].h, &p->missing);
    PdhCollectQueryData(p->q);                              /* baseline for rate counters */
    p->ok = true;
    return true;
}

void metrics_shutdown(PdhState *p) {
    for (int i = 0; i < WFT_PDH_ARRAYS; ++i) free(p->arr[i].buf);
    if (p->q) PdhCloseQuery(p->q);
    memset(p, 0, sizeof *p);
}

void metrics_collect(PdhState *p, Metrics *m) {
    metrics_reset_na(m);
    if (p->ok && PdhCollectQueryData(p->q) == ERROR_SUCCESS) {
        for (int i = 0; i < WFT_PDH_SINGLES; ++i) {
            double v = value_of(p->single[i]);
            *(double *)((char *)m + k_single[i].off) = WFT_HAS(v) ? v * k_single[i].scale : WFT_NA;
        }
        for (int i = 0; i < WFT_PDH_ARRAYS; ++i) {
            double v;
            char *name = k_arr[i].off_name == NONAME ? NULL : (char *)m + k_arr[i].off_name;
            if (read_array(&p->arr[i], k_arr[i].scale, true, &v, name, k_arr[i].name_cch))
                *(double *)((char *)m + k_arr[i].off_val) = v;
        }
    }
    MEMORYSTATUSEX ms; memset(&ms, 0, sizeof ms); ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        m->mem_load = (double)ms.dwMemoryLoad;
        m->avail_mb = (double)(ms.ullAvailPhys / (1024ull * 1024ull));
        m->commit_avail_mb = (double)(ms.ullAvailPageFile / (1024ull * 1024ull));
    }
    PERFORMANCE_INFORMATION pi; memset(&pi, 0, sizeof pi); pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        double pg = (double)pi.PageSize / 1048576.0;
        m->pool_nonpaged_mb = (double)pi.KernelNonpaged * pg;
        m->pool_paged_mb    = (double)pi.KernelPaged * pg;
        m->commit_used_mb   = (double)pi.CommitTotal * pg;
        m->commit_limit_mb  = (double)pi.CommitLimit * pg;
        m->system_handles   = (double)pi.HandleCount;
        m->system_threads   = (double)pi.ThreadCount;
        m->system_processes = (double)pi.ProcessCount;
    }
}

bool pdh_oneshot_extreme(const WCHAR *path, bool want_max, double *value, char *name, size_t name_cch) {
    PDH_HQUERY q = NULL; ArrayCounter a; memset(&a, 0, sizeof a);
    bool ok = false; int missing = 0;
    if (PdhOpenQueryW(NULL, 0, &q) != ERROR_SUCCESS) return false;
    add_counter(q, path, &a.h, &missing);
    if (a.h && PdhCollectQueryData(q) == ERROR_SUCCESS) ok = read_array(&a, 1.0, want_max, value, name, name_cch);
    free(a.buf);
    PdhCloseQuery(q);
    return ok;
}
