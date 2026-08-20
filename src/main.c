#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#include <winevt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_NAME L"WinFreezeTrace"
#define VERSION_W L"0.1.0"
#define MAX_DISKS 32
#define MAX_TOP_IO 5
#define MAX_PROCS 4096
#define MAX_PATH_WFT 1024
#define ROLLING_LIMIT_BYTES (32LL * 1024LL * 1024LL)
#define ROLLING_FLUSH_MS 2000ULL
#define EVENTLOG_WINDOW_MS 120000ULL
#define WATCHDOG_GAP_MS 2000ULL

#define TRIGGER_LATENCY 0x01u
#define TRIGGER_QUEUE   0x02u
#define TRIGGER_GAP     0x04u

#define COUNTOF(x) (sizeof(x) / sizeof((x)[0]))

typedef enum CollectorStage {
    STAGE_STARTING = 0,
    STAGE_SLEEP,
    STAGE_PDH_COLLECT,
    STAGE_PDH_FORMAT,
    STAGE_PROCESS_SCAN,
    STAGE_RING_UPDATE,
    STAGE_FILE_WRITE,
    STAGE_EVENT_FINALIZE,
    STAGE_STOPPED
} CollectorStage;

typedef struct Config {
    DWORD interval_ms;
    DWORD ring_minutes;
    DWORD pre_minutes;
    DWORD post_minutes;
    double latency_trigger_ms;
    double queue_trigger;
    bool process_io;
    bool quiet;
    wchar_t log_dir[MAX_PATH_WFT];
} Config;

typedef struct DiskMetric {
    double read_latency_ms;
    double write_latency_ms;
    double queue;
    double bytes_sec;
    double iops;
    double busy_pct;
    bool valid;
} DiskMetric;

typedef struct TopIo {
    DWORD pid;
    wchar_t name[MAX_PATH];
    uint64_t read_bps;
    uint64_t write_bps;
    uint64_t total_bps;
} TopIo;

typedef struct Sample {
    uint64_t seq;
    uint64_t mono_ms;
    FILETIME utc;
    double interval_ms;
    double pdh_ms;
    double proc_ms;
    double cpu_pct;
    double dpc_pct;
    double interrupt_pct;
    double avail_mb;
    double pages_sec;
    double process_count;
    double thread_count;
    double uptime_sec;
    uint32_t trigger_flags;
    uint32_t disk_mask;
    DiskMetric disks[MAX_DISKS];
    TopIo top[MAX_TOP_IO];
} Sample;

typedef struct ProcPrev {
    DWORD pid;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t stamp_ms;
    uint64_t seen_generation;
    wchar_t name[MAX_PATH];
} ProcPrev;

typedef struct PdhState {
    PDH_HQUERY query;
    PDH_HCOUNTER cpu;
    PDH_HCOUNTER dpc;
    PDH_HCOUNTER interrupt;
    PDH_HCOUNTER avail_mb;
    PDH_HCOUNTER pages_sec;
    PDH_HCOUNTER processes;
    PDH_HCOUNTER threads;
    PDH_HCOUNTER uptime;

    PDH_HCOUNTER disk_read_latency;
    PDH_HCOUNTER disk_write_latency;
    PDH_HCOUNTER disk_queue;
    PDH_HCOUNTER disk_bytes;
    PDH_HCOUNTER disk_iops;
    PDH_HCOUNTER disk_busy;
} PdhState;

typedef struct EventCapture {
    bool active;
    uint64_t trigger_mono_ms;
    FILETIME trigger_utc;
    uint32_t reasons;
    wchar_t dir[MAX_PATH_WFT];
    FILE *metrics;
    FILE *summary;
} EventCapture;

static Config g_cfg;
static volatile LONG g_stop = 0;
static volatile LONG64 g_heartbeat_ms = 0;
static volatile LONG g_stage = STAGE_STARTING;
static volatile LONG g_watchdog_trigger_pending = 0;
static volatile LONG64 g_watchdog_gap_ms = 0;
static CRITICAL_SECTION g_log_lock;

static wchar_t g_disk_names[MAX_DISKS][128];
static size_t g_disk_count = 0;

static Sample *g_ring = NULL;
static size_t g_ring_cap = 0;
static size_t g_ring_count = 0;
static size_t g_ring_head = 0;

static ProcPrev g_procs[MAX_PROCS];
static size_t g_proc_count = 0;
static uint64_t g_proc_generation = 0;
static uint64_t g_last_proc_scan_ms = 0;
static TopIo g_last_top[MAX_TOP_IO];

static FILE *g_rolling = NULL;
static wchar_t g_rolling_path[MAX_PATH_WFT];
static uint64_t g_last_roll_flush_ms = 0;
static EventCapture g_event;

static uint64_t mono_ms(void) {
    return GetTickCount64();
}

static void heartbeat(CollectorStage stage) {
    InterlockedExchange(&g_stage, (LONG)stage);
    InterlockedExchange64(&g_heartbeat_ms, (LONG64)mono_ms());
}

static const wchar_t *stage_name(LONG stage) {
    switch ((CollectorStage)stage) {
        case STAGE_STARTING: return L"starting";
        case STAGE_SLEEP: return L"sleep";
        case STAGE_PDH_COLLECT: return L"pdh_collect";
        case STAGE_PDH_FORMAT: return L"pdh_format";
        case STAGE_PROCESS_SCAN: return L"process_scan";
        case STAGE_RING_UPDATE: return L"ring_update";
        case STAGE_FILE_WRITE: return L"file_write";
        case STAGE_EVENT_FINALIZE: return L"event_finalize";
        case STAGE_STOPPED: return L"stopped";
        default: return L"unknown";
    }
}

static void filetime_to_local_text(FILETIME ft, wchar_t *buf, size_t cap) {
    FILETIME local_ft;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local_ft) || !FileTimeToSystemTime(&local_ft, &st)) {
        wcsncpy(buf, L"unknown", cap - 1);
        buf[cap - 1] = L'\0';
        return;
    }
    _snwprintf(buf, cap, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void filetime_to_folder_text(FILETIME ft, wchar_t *buf, size_t cap) {
    FILETIME local_ft;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local_ft);
    FileTimeToSystemTime(&local_ft, &st);
    _snwprintf(buf, cap, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static uint64_t filetime_to_u64(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static FILETIME u64_to_filetime(uint64_t v) {
    ULARGE_INTEGER u;
    FILETIME ft;
    u.QuadPart = v;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

static void filetime_to_iso_utc(FILETIME ft, wchar_t *buf, size_t cap) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    _snwprintf(buf, cap, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static bool ensure_dir(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (CreateDirectoryW(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static void write_utf8_w(FILE *f, const wchar_t *s) {
    if (!s) return;
    int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (needed <= 1) return;
    char stack[512];
    char *buf = stack;
    if ((size_t)needed > sizeof(stack)) {
        buf = (char *)malloc((size_t)needed);
        if (!buf) return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, needed, NULL, NULL) > 0) {
        fwrite(buf, 1, (size_t)needed - 1, f);
    }
    if (buf != stack) free(buf);
}

static void csv_write_quoted_w(FILE *f, const wchar_t *s) {
    fputc('"', f);
    if (s) {
        const wchar_t *start = s;
        for (const wchar_t *p = s; ; ++p) {
            if (*p == L'"' || *p == L'\0') {
                size_t n = (size_t)(p - start);
                if (n) {
                    wchar_t stack[256];
                    wchar_t *tmp = stack;
                    if (n + 1 > COUNTOF(stack)) {
                        tmp = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
                        if (!tmp) break;
                    }
                    wmemcpy(tmp, start, n);
                    tmp[n] = L'\0';
                    write_utf8_w(f, tmp);
                    if (tmp != stack) free(tmp);
                }
                if (*p == L'"') {
                    fputs("\"\"", f);
                    start = p + 1;
                } else break;
            }
        }
    }
    fputc('"', f);
}

static void log_line_locked(const wchar_t *name, const wchar_t *line) {
    wchar_t path[MAX_PATH_WFT];
    _snwprintf(path, COUNTOF(path), L"%ls\\%ls", g_cfg.log_dir, name);
    EnterCriticalSection(&g_log_lock);
    FILE *f = _wfopen(path, L"a, ccs=UTF-8");
    if (f) {
        fwprintf(f, L"%ls\n", line);
        fflush(f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

static void print_usage(void) {
    wprintf(L"%ls %ls\n\n", APP_NAME, VERSION_W);
    wprintf(L"Usage: WinFreezeTrace.exe [options]\n\n");
    wprintf(L"  --interval <ms>       Sample interval, default 500\n");
    wprintf(L"  --ring-minutes <n>    In-memory history, default 30\n");
    wprintf(L"  --pre-minutes <n>     Event history before trigger, default 5\n");
    wprintf(L"  --post-minutes <n>    Event capture after trigger, default 5\n");
    wprintf(L"  --latency-ms <n>      Trigger if disk read/write latency exceeds n, default 500\n");
    wprintf(L"  --queue <n>           Trigger if disk queue exceeds n, default 10\n");
    wprintf(L"  --log-dir <path>      Log directory, default .\\logs\n");
    wprintf(L"  --no-process-io       Disable top process I/O sampling\n");
    wprintf(L"  --quiet               Reduce console output\n");
    wprintf(L"  --help                 Show this help\n");
}

static bool parse_u32(const wchar_t *s, DWORD *out) {
    wchar_t *end = NULL;
    unsigned long v = wcstoul(s, &end, 10);
    if (!s[0] || (end && *end) || v > 0xffffffffUL) return false;
    *out = (DWORD)v;
    return true;
}

static bool parse_double_arg(const wchar_t *s, double *out) {
    wchar_t *end = NULL;
    double v = wcstod(s, &end);
    if (!s[0] || (end && *end) || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_args(int argc, wchar_t **argv) {
    g_cfg.interval_ms = 500;
    g_cfg.ring_minutes = 30;
    g_cfg.pre_minutes = 5;
    g_cfg.post_minutes = 5;
    g_cfg.latency_trigger_ms = 500.0;
    g_cfg.queue_trigger = 10.0;
    g_cfg.process_io = true;
    g_cfg.quiet = false;
    wcscpy(g_cfg.log_dir, L"logs");

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            print_usage();
            return false;
        } else if (!wcscmp(argv[i], L"--no-process-io")) {
            g_cfg.process_io = false;
        } else if (!wcscmp(argv[i], L"--quiet")) {
            g_cfg.quiet = true;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--interval")) {
            if (!parse_u32(argv[++i], &g_cfg.interval_ms)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--ring-minutes")) {
            if (!parse_u32(argv[++i], &g_cfg.ring_minutes)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--pre-minutes")) {
            if (!parse_u32(argv[++i], &g_cfg.pre_minutes)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--post-minutes")) {
            if (!parse_u32(argv[++i], &g_cfg.post_minutes)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--latency-ms")) {
            if (!parse_double_arg(argv[++i], &g_cfg.latency_trigger_ms)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--queue")) {
            if (!parse_double_arg(argv[++i], &g_cfg.queue_trigger)) return false;
        } else if (i + 1 < argc && !wcscmp(argv[i], L"--log-dir")) {
            wcsncpy(g_cfg.log_dir, argv[++i], COUNTOF(g_cfg.log_dir) - 1);
            g_cfg.log_dir[COUNTOF(g_cfg.log_dir) - 1] = L'\0';
        } else {
            fwprintf(stderr, L"Unknown/invalid option: %ls\n", argv[i]);
            return false;
        }
    }

    if (g_cfg.interval_ms < 100 || g_cfg.interval_ms > 10000 ||
        g_cfg.ring_minutes < 1 || g_cfg.ring_minutes > 240 ||
        g_cfg.pre_minutes > g_cfg.ring_minutes || g_cfg.post_minutes > 60 ||
        g_cfg.latency_trigger_ms < 1.0 || g_cfg.queue_trigger < 0.0) {
        fwprintf(stderr, L"Invalid option range. Use --help.\n");
        return false;
    }
    return true;
}

static PDH_STATUS add_counter(PDH_HQUERY q, const wchar_t *path, PDH_HCOUNTER *out) {
    return PdhAddEnglishCounterW(q, path, 0, out);
}

static bool pdh_init(PdhState *p) {
    ZeroMemory(p, sizeof(*p));
    if (PdhOpenQueryW(NULL, 0, &p->query) != ERROR_SUCCESS) return false;

    struct Item { const wchar_t *path; PDH_HCOUNTER *dst; } items[] = {
        {L"\\Processor(_Total)\\% Processor Time", &p->cpu},
        {L"\\Processor(_Total)\\% DPC Time", &p->dpc},
        {L"\\Processor(_Total)\\% Interrupt Time", &p->interrupt},
        {L"\\Memory\\Available MBytes", &p->avail_mb},
        {L"\\Memory\\Pages/sec", &p->pages_sec},
        {L"\\System\\Processes", &p->processes},
        {L"\\System\\Threads", &p->threads},
        {L"\\System\\System Up Time", &p->uptime},
        {L"\\PhysicalDisk(*)\\Avg. Disk sec/Read", &p->disk_read_latency},
        {L"\\PhysicalDisk(*)\\Avg. Disk sec/Write", &p->disk_write_latency},
        {L"\\PhysicalDisk(*)\\Current Disk Queue Length", &p->disk_queue},
        {L"\\PhysicalDisk(*)\\Disk Bytes/sec", &p->disk_bytes},
        {L"\\PhysicalDisk(*)\\Disk Transfers/sec", &p->disk_iops},
        {L"\\PhysicalDisk(*)\\% Disk Time", &p->disk_busy},
    };

    for (size_t i = 0; i < COUNTOF(items); ++i) {
        PDH_STATUS st = add_counter(p->query, items[i].path, items[i].dst);
        if (st != ERROR_SUCCESS) {
            fwprintf(stderr, L"PDH add counter failed 0x%08lx: %ls\n", (unsigned long)st, items[i].path);
            PdhCloseQuery(p->query);
            ZeroMemory(p, sizeof(*p));
            return false;
        }
    }

    PdhCollectQueryData(p->query);
    Sleep(200);
    PdhCollectQueryData(p->query);
    return true;
}

static void pdh_close(PdhState *p) {
    if (p->query) PdhCloseQuery(p->query);
    ZeroMemory(p, sizeof(*p));
}

static bool pdh_status_has_data(DWORD status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

static double pdh_value(PDH_HCOUNTER c) {
    PDH_FMT_COUNTERVALUE v;
    DWORD type = 0;
    PDH_STATUS st = PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, &type, &v);
    if (st != ERROR_SUCCESS || !pdh_status_has_data(v.CStatus)) return NAN;
    return v.doubleValue;
}

typedef struct CounterArray {
    PPDH_FMT_COUNTERVALUE_ITEM_W items;
    DWORD count;
} CounterArray;

static bool pdh_array(PDH_HCOUNTER c, CounterArray *a) {
    DWORD size = 0;
    DWORD count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, NULL);
    if (st != PDH_MORE_DATA || size == 0) return false;
    PPDH_FMT_COUNTERVALUE_ITEM_W items = (PPDH_FMT_COUNTERVALUE_ITEM_W)malloc(size);
    if (!items) return false;
    st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items);
    if (st != ERROR_SUCCESS) {
        free(items);
        return false;
    }
    a->items = items;
    a->count = count;
    return true;
}

static void pdh_array_free(CounterArray *a) {
    free(a->items);
    a->items = NULL;
    a->count = 0;
}

static double array_lookup(const CounterArray *a, const wchar_t *name) {
    if (!a->items) return NAN;
    for (DWORD i = 0; i < a->count; ++i) {
        if (!wcscmp(a->items[i].szName, name)) {
            if (!pdh_status_has_data(a->items[i].FmtValue.CStatus)) return NAN;
            return a->items[i].FmtValue.doubleValue;
        }
    }
    return NAN;
}

static int disk_index_for_name(const wchar_t *name) {
    for (size_t i = 0; i < g_disk_count; ++i) {
        if (!wcscmp(g_disk_names[i], name)) return (int)i;
    }
    if (g_disk_count >= MAX_DISKS) return -1;
    wcsncpy(g_disk_names[g_disk_count], name, COUNTOF(g_disk_names[0]) - 1);
    g_disk_names[g_disk_count][COUNTOF(g_disk_names[0]) - 1] = L'\0';
    return (int)g_disk_count++;
}

static void collect_disk_arrays(PdhState *p, Sample *s) {
    CounterArray r = {0}, w = {0}, q = {0}, b = {0}, io = {0}, busy = {0};
    if (!pdh_array(p->disk_read_latency, &r)) goto done;
    pdh_array(p->disk_write_latency, &w);
    pdh_array(p->disk_queue, &q);
    pdh_array(p->disk_bytes, &b);
    pdh_array(p->disk_iops, &io);
    pdh_array(p->disk_busy, &busy);

    for (DWORD i = 0; i < r.count; ++i) {
        const wchar_t *name = r.items[i].szName;
        if (!name || !name[0]) continue;
        int idx = disk_index_for_name(name);
        if (idx < 0 || idx >= MAX_DISKS) continue;
        DiskMetric *d = &s->disks[idx];
        if (pdh_status_has_data(r.items[i].FmtValue.CStatus)) {
            d->read_latency_ms = r.items[i].FmtValue.doubleValue * 1000.0;
        } else d->read_latency_ms = NAN;
        d->write_latency_ms = array_lookup(&w, name) * 1000.0;
        d->queue = array_lookup(&q, name);
        d->bytes_sec = array_lookup(&b, name);
        d->iops = array_lookup(&io, name);
        d->busy_pct = array_lookup(&busy, name);
        d->valid = true;
        s->disk_mask |= (1u << idx);
    }

done:
    pdh_array_free(&r); pdh_array_free(&w); pdh_array_free(&q);
    pdh_array_free(&b); pdh_array_free(&io); pdh_array_free(&busy);
}

static ProcPrev *find_proc_prev(DWORD pid) {
    for (size_t i = 0; i < g_proc_count; ++i) if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

static ProcPrev *get_or_add_proc(DWORD pid, const wchar_t *name) {
    ProcPrev *p = find_proc_prev(pid);
    if (p) return p;
    if (g_proc_count >= MAX_PROCS) return NULL;
    p = &g_procs[g_proc_count++];
    ZeroMemory(p, sizeof(*p));
    p->pid = pid;
    wcsncpy(p->name, name, COUNTOF(p->name) - 1);
    return p;
}

static void insert_top_io(TopIo top[MAX_TOP_IO], DWORD pid, const wchar_t *name,
                          uint64_t read_bps, uint64_t write_bps) {
    TopIo x;
    ZeroMemory(&x, sizeof(x));
    x.pid = pid;
    wcsncpy(x.name, name, COUNTOF(x.name) - 1);
    x.read_bps = read_bps;
    x.write_bps = write_bps;
    x.total_bps = read_bps + write_bps;
    if (x.total_bps == 0) return;

    for (size_t i = 0; i < MAX_TOP_IO; ++i) {
        if (x.total_bps > top[i].total_bps) {
            for (size_t j = MAX_TOP_IO - 1; j > i; --j) top[j] = top[j - 1];
            top[i] = x;
            break;
        }
    }
}

static void collect_process_io(Sample *s) {
    if (!g_cfg.process_io) return;
    uint64_t now = mono_ms();
    if (g_last_proc_scan_ms && now - g_last_proc_scan_ms < 1000) {
        memcpy(s->top, g_last_top, sizeof(g_last_top));
        return;
    }
    g_last_proc_scan_ms = now;
    ++g_proc_generation;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid == 0) continue;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) continue;
        IO_COUNTERS io;
        if (GetProcessIoCounters(hp, &io)) {
            ProcPrev *prev = get_or_add_proc(pid, pe.szExeFile);
            if (prev) {
                if (prev->stamp_ms && now > prev->stamp_ms &&
                    io.ReadTransferCount >= prev->read_bytes && io.WriteTransferCount >= prev->write_bytes) {
                    uint64_t dt = now - prev->stamp_ms;
                    uint64_t rbps = (io.ReadTransferCount - prev->read_bytes) * 1000ULL / dt;
                    uint64_t wbps = (io.WriteTransferCount - prev->write_bytes) * 1000ULL / dt;
                    insert_top_io(s->top, pid, pe.szExeFile, rbps, wbps);
                }
                prev->read_bytes = io.ReadTransferCount;
                prev->write_bytes = io.WriteTransferCount;
                prev->stamp_ms = now;
                prev->seen_generation = g_proc_generation;
                wcsncpy(prev->name, pe.szExeFile, COUNTOF(prev->name) - 1);
            }
        }
        CloseHandle(hp);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    if ((g_proc_generation % 120) == 0) {
        size_t out = 0;
        for (size_t i = 0; i < g_proc_count; ++i) {
            if (g_proc_generation - g_procs[i].seen_generation < 120) {
                if (out != i) g_procs[out] = g_procs[i];
                ++out;
            }
        }
        g_proc_count = out;
    }
    memcpy(g_last_top, s->top, sizeof(g_last_top));
}

static void ring_push(const Sample *s) {
    g_ring[g_ring_head] = *s;
    g_ring_head = (g_ring_head + 1) % g_ring_cap;
    if (g_ring_count < g_ring_cap) ++g_ring_count;
}

static void write_csv_header(FILE *f) {
    fprintf(f, "timestamp,seq,interval_ms,pdh_ms,proc_ms,cpu_pct,dpc_pct,interrupt_pct,available_mb,pages_sec,process_count,thread_count,uptime_sec,trigger_flags,disk,read_latency_ms,write_latency_ms,queue,disk_bytes_sec,iops,busy_pct,top1_pid,top1_name,top1_read_bps,top1_write_bps,top2_pid,top2_name,top2_read_bps,top2_write_bps\n");
}

static void write_sample_rows(FILE *f, const Sample *s) {
    wchar_t ts[64];
    filetime_to_local_text(s->utc, ts, COUNTOF(ts));
    bool any = false;

    for (size_t i = 0; i < g_disk_count && i < MAX_DISKS; ++i) {
        if (!(s->disk_mask & (1u << i))) continue;
        any = true;
        const DiskMetric *d = &s->disks[i];
        write_utf8_w(f, ts);
        fprintf(f, ",%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.3f,%.0f,%.0f,%.3f,%u,",
                (unsigned long long)s->seq, s->interval_ms, s->pdh_ms, s->proc_ms,
                s->cpu_pct, s->dpc_pct, s->interrupt_pct, s->avail_mb, s->pages_sec,
                s->process_count, s->thread_count, s->uptime_sec, s->trigger_flags);
        csv_write_quoted_w(f, g_disk_names[i]);
        fprintf(f, ",%.3f,%.3f,%.3f,%.0f,%.3f,%.3f,%lu,",
                d->read_latency_ms, d->write_latency_ms, d->queue, d->bytes_sec, d->iops, d->busy_pct,
                (unsigned long)s->top[0].pid);
        csv_write_quoted_w(f, s->top[0].name);
        fprintf(f, ",%llu,%llu,%lu,",
                (unsigned long long)s->top[0].read_bps, (unsigned long long)s->top[0].write_bps,
                (unsigned long)s->top[1].pid);
        csv_write_quoted_w(f, s->top[1].name);
        fprintf(f, ",%llu,%llu\n",
                (unsigned long long)s->top[1].read_bps, (unsigned long long)s->top[1].write_bps);
    }

    if (!any) {
        write_utf8_w(f, ts);
        fprintf(f, ",%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.3f,%.0f,%.0f,%.3f,%u,\"(none)\",,,,,,,%lu,",
                (unsigned long long)s->seq, s->interval_ms, s->pdh_ms, s->proc_ms,
                s->cpu_pct, s->dpc_pct, s->interrupt_pct, s->avail_mb, s->pages_sec,
                s->process_count, s->thread_count, s->uptime_sec, s->trigger_flags,
                (unsigned long)s->top[0].pid);
        csv_write_quoted_w(f, s->top[0].name);
        fprintf(f, ",%llu,%llu,%lu,",
                (unsigned long long)s->top[0].read_bps, (unsigned long long)s->top[0].write_bps,
                (unsigned long)s->top[1].pid);
        csv_write_quoted_w(f, s->top[1].name);
        fprintf(f, ",%llu,%llu\n",
                (unsigned long long)s->top[1].read_bps, (unsigned long long)s->top[1].write_bps);
    }
}

static void close_rolling(void) {
    EnterCriticalSection(&g_log_lock);
    if (g_rolling) {
        fflush(g_rolling);
        fclose(g_rolling);
        g_rolling = NULL;
    }
    LeaveCriticalSection(&g_log_lock);
}

static bool open_rolling(void) {
    _snwprintf(g_rolling_path, COUNTOF(g_rolling_path), L"%ls\\rolling.csv", g_cfg.log_dir);
    bool new_file = GetFileAttributesW(g_rolling_path) == INVALID_FILE_ATTRIBUTES;
    g_rolling = _wfopen(g_rolling_path, L"a+");
    if (!g_rolling) return false;
    _fseeki64(g_rolling, 0, SEEK_END);
    if (new_file || _ftelli64(g_rolling) == 0) write_csv_header(g_rolling);
    return true;
}

static void rotate_rolling_if_needed(void) {
    if (!g_rolling) return;
    int64_t pos = _ftelli64(g_rolling);
    if (pos < ROLLING_LIMIT_BYTES) return;

    fflush(g_rolling);
    fclose(g_rolling);
    g_rolling = NULL;

    wchar_t prev[MAX_PATH_WFT];
    _snwprintf(prev, COUNTOF(prev), L"%ls\\rolling.prev.csv", g_cfg.log_dir);
    DeleteFileW(prev);
    MoveFileExW(g_rolling_path, prev, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    open_rolling();
}

static void rolling_write(const Sample *s) {
    EnterCriticalSection(&g_log_lock);
    if (!g_rolling && !open_rolling()) {
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    write_sample_rows(g_rolling, s);
    uint64_t now = mono_ms();
    if (now - g_last_roll_flush_ms >= ROLLING_FLUSH_MS || s->trigger_flags) {
        fflush(g_rolling);
        g_last_roll_flush_ms = now;
    }
    rotate_rolling_if_needed();
    LeaveCriticalSection(&g_log_lock);
}

static void reason_text(uint32_t flags, wchar_t *buf, size_t cap) {
    buf[0] = L'\0';
    if (flags & TRIGGER_LATENCY) wcsncat(buf, L"disk_latency ", cap - wcslen(buf) - 1);
    if (flags & TRIGGER_QUEUE) wcsncat(buf, L"disk_queue ", cap - wcslen(buf) - 1);
    if (flags & TRIGGER_GAP) wcsncat(buf, L"collector_gap ", cap - wcslen(buf) - 1);
    if (!buf[0]) wcsncpy(buf, L"unknown", cap - 1);
}

static void dump_ring_pre_history(FILE *f, uint64_t now_ms) {
    if (!g_ring_count) return;
    uint64_t pre_ms = (uint64_t)g_cfg.pre_minutes * 60ULL * 1000ULL;
    size_t oldest = (g_ring_head + g_ring_cap - g_ring_count) % g_ring_cap;
    for (size_t n = 0; n < g_ring_count; ++n) {
        size_t idx = (oldest + n) % g_ring_cap;
        const Sample *s = &g_ring[idx];
        if (s->mono_ms < now_ms && now_ms - s->mono_ms <= pre_ms) write_sample_rows(f, s);
    }
}

static bool start_event_capture(const Sample *trigger_sample, uint32_t reasons) {
    if (g_event.active) {
        g_event.reasons |= reasons;
        return true;
    }

    wchar_t stamp[64];
    filetime_to_folder_text(trigger_sample->utc, stamp, COUNTOF(stamp));
    _snwprintf(g_event.dir, COUNTOF(g_event.dir), L"%ls\\event-%ls", g_cfg.log_dir, stamp);
    if (!ensure_dir(g_event.dir)) return false;

    wchar_t path[MAX_PATH_WFT];
    _snwprintf(path, COUNTOF(path), L"%ls\\metrics.csv", g_event.dir);
    g_event.metrics = _wfopen(path, L"w");
    if (!g_event.metrics) return false;
    write_csv_header(g_event.metrics);

    _snwprintf(path, COUNTOF(path), L"%ls\\summary.txt", g_event.dir);
    g_event.summary = _wfopen(path, L"w, ccs=UTF-8");

    g_event.active = true;
    g_event.trigger_mono_ms = trigger_sample->mono_ms;
    g_event.trigger_utc = trigger_sample->utc;
    g_event.reasons = reasons;
    dump_ring_pre_history(g_event.metrics, trigger_sample->mono_ms);
    fflush(g_event.metrics);

    if (g_event.summary) {
        wchar_t ts[64], why[128];
        filetime_to_local_text(trigger_sample->utc, ts, COUNTOF(ts));
        reason_text(reasons, why, COUNTOF(why));
        fwprintf(g_event.summary, L"WinFreezeTrace %ls\nTrigger: %ls\nReasons: %ls\n", VERSION_W, ts, why);
        fflush(g_event.summary);
    }

    if (!g_cfg.quiet) wprintf(L"[trigger] %ls\n", g_event.dir);
    return true;
}

static bool xml_contains_provider(const wchar_t *xml) {
    static const wchar_t *needles[] = {
        L"Provider Name='disk'", L"Provider Name=\"disk\"",
        L"Provider Name='storahci'", L"Provider Name=\"storahci\"",
        L"Provider Name='Ntfs'", L"Provider Name=\"Ntfs\"",
        L"Provider Name='Microsoft-Windows-WHEA-Logger'", L"Provider Name=\"Microsoft-Windows-WHEA-Logger\"",
        L"Provider Name='Display'", L"Provider Name=\"Display\"",
        L"Provider Name='nvlddmkm'", L"Provider Name=\"nvlddmkm\"",
        L"Provider Name='Microsoft-Windows-Kernel-Power'", L"Provider Name=\"Microsoft-Windows-Kernel-Power\""
    };
    for (size_t i = 0; i < COUNTOF(needles); ++i) if (wcsstr(xml, needles[i])) return true;
    return false;
}

static void export_eventlog_window(const wchar_t *dir, FILETIME trigger) {
    const uint64_t ticks_per_ms = 10000ULL;
    uint64_t t = filetime_to_u64(trigger);
    uint64_t delta = EVENTLOG_WINDOW_MS * ticks_per_ms;
    FILETIME from = u64_to_filetime(t > delta ? t - delta : 0);
    FILETIME to = u64_to_filetime(t + delta);
    wchar_t from_iso[64], to_iso[64], xpath[512];
    filetime_to_iso_utc(from, from_iso, COUNTOF(from_iso));
    filetime_to_iso_utc(to, to_iso, COUNTOF(to_iso));
    _snwprintf(xpath, COUNTOF(xpath),
               L"*[System[TimeCreated[@SystemTime >= '%ls' and @SystemTime <= '%ls']]]", from_iso, to_iso);

    EVT_HANDLE q = EvtQuery(NULL, L"System", xpath, EvtQueryChannelPath | EvtQueryForwardDirection);
    wchar_t path[MAX_PATH_WFT];
    _snwprintf(path, COUNTOF(path), L"%ls\\eventlog.xml", dir);
    FILE *out = _wfopen(path, L"w, ccs=UTF-8");
    if (!out) {
        if (q) EvtClose(q);
        return;
    }
    fwprintf(out, L"<!-- System log filtered to disk/storahci/Ntfs/WHEA/Display/nvlddmkm/Kernel-Power, trigger +/- 2 min -->\n<Events>\n");

    if (q) {
        EVT_HANDLE ev[32];
        DWORD returned = 0;
        while (EvtNext(q, (DWORD)COUNTOF(ev), ev, 0, 0, &returned)) {
            for (DWORD i = 0; i < returned; ++i) {
                DWORD used = 0, props = 0;
                EvtRender(NULL, ev[i], EvtRenderEventXml, 0, NULL, &used, &props);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used) {
                    wchar_t *xml = (wchar_t *)malloc(used);
                    if (xml && EvtRender(NULL, ev[i], EvtRenderEventXml, used, xml, &used, &props)) {
                        if (xml_contains_provider(xml)) fwprintf(out, L"%ls\n", xml);
                    }
                    free(xml);
                }
                EvtClose(ev[i]);
            }
        }
        EvtClose(q);
    }
    fwprintf(out, L"</Events>\n");
    fclose(out);
}

static void finalize_event_capture(void) {
    if (!g_event.active) return;
    heartbeat(STAGE_EVENT_FINALIZE);
    if (g_event.metrics) {
        fflush(g_event.metrics);
        fclose(g_event.metrics);
        g_event.metrics = NULL;
    }
    if (g_event.summary) {
        wchar_t why[128];
        reason_text(g_event.reasons, why, COUNTOF(why));
        fwprintf(g_event.summary, L"Final reasons: %ls\nCapture completed normally.\n", why);
        fflush(g_event.summary);
        fclose(g_event.summary);
        g_event.summary = NULL;
    }
    export_eventlog_window(g_event.dir, g_event.trigger_utc);
    if (!g_cfg.quiet) wprintf(L"[event saved] %ls\n", g_event.dir);
    ZeroMemory(&g_event, sizeof(g_event));
}

static uint32_t evaluate_triggers(const Sample *s) {
    uint32_t flags = 0;
    for (size_t i = 0; i < g_disk_count && i < MAX_DISKS; ++i) {
        if (!(s->disk_mask & (1u << i))) continue;
        if (!wcscmp(g_disk_names[i], L"_Total")) continue;
        const DiskMetric *d = &s->disks[i];
        if ((isfinite(d->read_latency_ms) && d->read_latency_ms >= g_cfg.latency_trigger_ms) ||
            (isfinite(d->write_latency_ms) && d->write_latency_ms >= g_cfg.latency_trigger_ms)) {
            flags |= TRIGGER_LATENCY;
        }
        if (isfinite(d->queue) && d->queue >= g_cfg.queue_trigger) flags |= TRIGGER_QUEUE;
    }
    if (InterlockedExchange(&g_watchdog_trigger_pending, 0)) flags |= TRIGGER_GAP;
    return flags;
}

static void event_append(const Sample *s) {
    if (!g_event.active || !g_event.metrics) return;
    write_sample_rows(g_event.metrics, s);
    fflush(g_event.metrics);
    uint64_t post_ms = (uint64_t)g_cfg.post_minutes * 60ULL * 1000ULL;
    if (s->mono_ms >= g_event.trigger_mono_ms && s->mono_ms - g_event.trigger_mono_ms >= post_ms) {
        finalize_event_capture();
    }
}

static DWORD WINAPI watchdog_thread(LPVOID unused) {
    (void)unused;
    bool gap_reported = false;
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        Sleep(250);
        uint64_t now = mono_ms();
        uint64_t hb = (uint64_t)InterlockedCompareExchange64(&g_heartbeat_ms, 0, 0);
        if (!hb) continue;
        uint64_t gap = now - hb;
        if (gap >= WATCHDOG_GAP_MS && !gap_reported) {
            LONG st = InterlockedCompareExchange(&g_stage, 0, 0);
            if (st == STAGE_EVENT_FINALIZE || st == STAGE_STOPPED) continue;
            wchar_t line[512], ts[64];
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            filetime_to_local_text(ft, ts, COUNTOF(ts));
            _snwprintf(line, COUNTOF(line), L"%ls gap_ms=%llu collector_stage=%ls",
                       ts, (unsigned long long)gap, stage_name(st));
            log_line_locked(L"watchdog.log", line);
            InterlockedExchange64(&g_watchdog_gap_ms, (LONG64)gap);
            InterlockedExchange(&g_watchdog_trigger_pending, 1);
            gap_reported = true;
        } else if (gap < 1000) {
            gap_reported = false;
        }
    }
    return 0;
}

static void print_status(const Sample *s) {
    if (g_cfg.quiet) return;
    double worst = 0.0, qmax = 0.0;
    const wchar_t *name = L"-";
    for (size_t i = 0; i < g_disk_count && i < MAX_DISKS; ++i) {
        if (!(s->disk_mask & (1u << i))) continue;
        if (!wcscmp(g_disk_names[i], L"_Total")) continue;
        double r = isfinite(s->disks[i].read_latency_ms) ? s->disks[i].read_latency_ms : 0.0;
        double w = isfinite(s->disks[i].write_latency_ms) ? s->disks[i].write_latency_ms : 0.0;
        double m = r > w ? r : w;
        if (m > worst) { worst = m; name = g_disk_names[i]; }
        if (isfinite(s->disks[i].queue) && s->disks[i].queue > qmax) qmax = s->disks[i].queue;
    }
    wprintf(L"\rCPU %5.1f%%  DPC %5.1f%%  disk worst %8.1f ms %-18ls  q %5.1f  PDH %6.1f ms   ",
            s->cpu_pct, s->dpc_pct, worst, name, qmax, s->pdh_ms);
    fflush(stdout);
}

int wmain(int argc, wchar_t **argv) {
    if (!parse_args(argc, argv)) {
        if (argc > 1 && (!wcscmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h"))) return 0;
        return 2;
    }

    InitializeCriticalSection(&g_log_lock);
    SetConsoleCtrlHandler(console_handler, TRUE);
    if (!ensure_dir(g_cfg.log_dir)) {
        fwprintf(stderr, L"Cannot create log directory: %ls (error %lu)\n", g_cfg.log_dir, GetLastError());
        DeleteCriticalSection(&g_log_lock);
        return 3;
    }

    uint64_t ring_ms = (uint64_t)g_cfg.ring_minutes * 60ULL * 1000ULL;
    g_ring_cap = (size_t)(ring_ms / g_cfg.interval_ms) + 16;
    g_ring = (Sample *)calloc(g_ring_cap, sizeof(Sample));
    if (!g_ring) {
        fwprintf(stderr, L"Cannot allocate ring buffer.\n");
        DeleteCriticalSection(&g_log_lock);
        return 4;
    }

    PdhState pdh;
    if (!pdh_init(&pdh)) {
        fwprintf(stderr, L"PDH initialization failed.\n");
        free(g_ring);
        DeleteCriticalSection(&g_log_lock);
        return 5;
    }

    if (!open_rolling()) {
        fwprintf(stderr, L"Warning: cannot open rolling.csv; watchdog.log may still work.\n");
    }

    heartbeat(STAGE_STARTING);
    HANDLE watchdog = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);

    if (!g_cfg.quiet) {
        wprintf(L"%ls %ls\n", APP_NAME, VERSION_W);
        wprintf(L"interval=%lu ms, ring=%lu min, trigger latency=%.0f ms, queue=%.1f\n",
                (unsigned long)g_cfg.interval_ms, (unsigned long)g_cfg.ring_minutes,
                g_cfg.latency_trigger_ms, g_cfg.queue_trigger);
        wprintf(L"logs: %ls\nPress Ctrl+C to stop.\n\n", g_cfg.log_dir);
    }

    uint64_t seq = 0;
    uint64_t last_sample_ms = mono_ms();
    uint64_t next_due = last_sample_ms;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        uint64_t now = mono_ms();
        if (now < next_due) {
            heartbeat(STAGE_SLEEP);
            DWORD sleep_ms = (DWORD)(next_due - now);
            Sleep(sleep_ms > 50 ? 50 : sleep_ms);
            continue;
        }

        Sample s;
        ZeroMemory(&s, sizeof(s));
        s.seq = ++seq;
        s.mono_ms = mono_ms();
        GetSystemTimeAsFileTime(&s.utc);
        s.interval_ms = (double)(s.mono_ms - last_sample_ms);
        last_sample_ms = s.mono_ms;

        uint64_t t0 = mono_ms();
        heartbeat(STAGE_PDH_COLLECT);
        PDH_STATUS st = PdhCollectQueryData(pdh.query);
        uint64_t t1 = mono_ms();
        s.pdh_ms = (double)(t1 - t0);
        if (st != ERROR_SUCCESS) {
            wchar_t line[256], ts[64];
            filetime_to_local_text(s.utc, ts, COUNTOF(ts));
            _snwprintf(line, COUNTOF(line), L"%ls PdhCollectQueryData failed 0x%08lx", ts, (unsigned long)st);
            log_line_locked(L"errors.log", line);
        }

        heartbeat(STAGE_PDH_FORMAT);
        s.cpu_pct = pdh_value(pdh.cpu);
        s.dpc_pct = pdh_value(pdh.dpc);
        s.interrupt_pct = pdh_value(pdh.interrupt);
        s.avail_mb = pdh_value(pdh.avail_mb);
        s.pages_sec = pdh_value(pdh.pages_sec);
        s.process_count = pdh_value(pdh.processes);
        s.thread_count = pdh_value(pdh.threads);
        s.uptime_sec = pdh_value(pdh.uptime);
        collect_disk_arrays(&pdh, &s);

        uint64_t p0 = mono_ms();
        heartbeat(STAGE_PROCESS_SCAN);
        collect_process_io(&s);
        s.proc_ms = (double)(mono_ms() - p0);

        s.trigger_flags = evaluate_triggers(&s);

        heartbeat(STAGE_RING_UPDATE);
        ring_push(&s);

        if (s.trigger_flags) start_event_capture(&s, s.trigger_flags);

        heartbeat(STAGE_FILE_WRITE);
        rolling_write(&s);
        event_append(&s);
        print_status(&s);

        heartbeat(STAGE_SLEEP);
        next_due += g_cfg.interval_ms;
        uint64_t after = mono_ms();
        if (after > next_due + (uint64_t)g_cfg.interval_ms * 4ULL) next_due = after + g_cfg.interval_ms;
    }

    if (!g_cfg.quiet) wprintf(L"\nStopping...\n");
    if (g_event.active) {
        if (g_event.summary) {
            fwprintf(g_event.summary, L"Capture interrupted/stopped before post-window completed.\n");
            fflush(g_event.summary);
        }
        finalize_event_capture();
    }
    heartbeat(STAGE_STOPPED);
    if (watchdog) {
        WaitForSingleObject(watchdog, 2000);
        CloseHandle(watchdog);
    }
    close_rolling();
    pdh_close(&pdh);
    free(g_ring);
    DeleteCriticalSection(&g_log_lock);
    return 0;
}
