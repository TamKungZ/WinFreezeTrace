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
#define VERSION_W L"0.2.1"
#define SERVICE_NAME L"WinFreezeTrace"
#define SERVICE_DISPLAY_NAME L"WinFreezeTrace Freeze Monitor"
#define INSTALL_DIR_NAME L"WinFreezeTrace"
#define INSTALL_EXE_NAME L"winfreezetrace.exe"
#define MACHINE_ENV_KEY L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
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
static SERVICE_STATUS_HANDLE g_service_status_handle = NULL;
static SERVICE_STATUS g_service_status;
static int g_main_argc = 0;
static wchar_t **g_main_argv = NULL;

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

static bool ensure_dir_recursive(const wchar_t *path) {
    if (!path || !path[0]) return false;
    wchar_t tmp[MAX_PATH_WFT];
    wcsncpy(tmp, path, COUNTOF(tmp) - 1);
    tmp[COUNTOF(tmp) - 1] = L'\0';

    size_t len = wcslen(tmp);
    while (len > 0 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/')) tmp[--len] = L'\0';
    if (len == 0) return false;

    size_t start = 0;
    if (len >= 3 && tmp[1] == L':' && (tmp[2] == L'\\' || tmp[2] == L'/')) {
        start = 3;
    } else if (len >= 2 && tmp[0] == L'\\' && tmp[1] == L'\\') {
        int separators = 0;
        for (size_t i = 2; i < len; ++i) {
            if (tmp[i] == L'\\' || tmp[i] == L'/') {
                separators++;
                if (separators == 2) { start = i + 1; break; }
            }
        }
    }

    for (size_t i = start; i < len; ++i) {
        if (tmp[i] == L'/' ) tmp[i] = L'\\';
        if (tmp[i] == L'\\') {
            wchar_t saved = tmp[i];
            tmp[i] = L'\0';
            if (tmp[0] && !ensure_dir(tmp)) return false;
            tmp[i] = saved;
        }
    }
    return ensure_dir(tmp);
}

static void default_log_dir(wchar_t *dst, size_t cap, bool service_mode) {
    if (!service_mode) {
        wcsncpy(dst, L"logs", cap - 1);
        dst[cap - 1] = L'\0';
        return;
    }

    wchar_t program_data[MAX_PATH_WFT];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", program_data, (DWORD)COUNTOF(program_data));
    if (n == 0 || n >= COUNTOF(program_data)) {
        wcsncpy(program_data, L"C:\\ProgramData", COUNTOF(program_data) - 1);
        program_data[COUNTOF(program_data) - 1] = L'\0';
    }
    _snwprintf(dst, cap, L"%ls\\WinFreezeTrace\\logs", program_data);
    dst[cap - 1] = L'\0';
}

static bool make_absolute_path(wchar_t *path, size_t cap) {
    if (!path || !path[0]) return false;
    wchar_t full[MAX_PATH_WFT];
    DWORD n = GetFullPathNameW(path, (DWORD)COUNTOF(full), full, NULL);
    if (n == 0 || n >= COUNTOF(full)) return false;
    wcsncpy(path, full, cap - 1);
    path[cap - 1] = L'\0';
    return true;
}


static bool get_install_paths(wchar_t *dir, size_t dir_cap, wchar_t *exe, size_t exe_cap) {
    wchar_t program_files[MAX_PATH_WFT];
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", program_files, (DWORD)COUNTOF(program_files));
    if (n == 0 || n >= COUNTOF(program_files)) {
        wcsncpy(program_files, L"C:\\Program Files", COUNTOF(program_files) - 1);
        program_files[COUNTOF(program_files) - 1] = L'\0';
    }
    _snwprintf(dir, dir_cap, L"%ls\\%ls", program_files, INSTALL_DIR_NAME);
    dir[dir_cap - 1] = L'\0';
    _snwprintf(exe, exe_cap, L"%ls\\%ls", dir, INSTALL_EXE_NAME);
    exe[exe_cap - 1] = L'\0';
    return dir[0] && exe[0];
}

static bool path_token_equal(const wchar_t *token, size_t token_len, const wchar_t *dir) {
    while (token_len && (*token == L' ' || *token == L'\t')) { token++; token_len--; }
    while (token_len && (token[token_len - 1] == L' ' || token[token_len - 1] == L'\t')) token_len--;
    if (token_len >= 2 && token[0] == L'"' && token[token_len - 1] == L'"') {
        token++; token_len -= 2;
    }
    while (token_len > 3 && (token[token_len - 1] == L'\\' || token[token_len - 1] == L'/')) token_len--;
    size_t dir_len = wcslen(dir);
    while (dir_len > 3 && (dir[dir_len - 1] == L'\\' || dir[dir_len - 1] == L'/')) dir_len--;
    return token_len == dir_len && _wcsnicmp(token, dir, dir_len) == 0;
}

static bool path_list_contains(const wchar_t *path, const wchar_t *dir) {
    if (!path || !dir) return false;
    const wchar_t *start = path;
    for (const wchar_t *p = path; ; ++p) {
        if (*p == L';' || *p == L'\0') {
            if (path_token_equal(start, (size_t)(p - start), dir)) return true;
            if (*p == L'\0') break;
            start = p + 1;
        }
    }
    return false;
}

static void broadcast_environment_change(void) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, &ignored);
}

static bool update_machine_path(const wchar_t *dir, bool add, bool *changed) {
    if (changed) *changed = false;
    HKEY key = NULL;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_ENV_KEY, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    rc = RegQueryValueExW(key, L"Path", NULL, &type, NULL, &bytes);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        SetLastError((DWORD)rc);
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) type = REG_EXPAND_SZ;
    if (bytes < sizeof(wchar_t)) bytes = sizeof(wchar_t);

    size_t cap_chars = (size_t)(bytes / sizeof(wchar_t)) + wcslen(dir) + 8;
    wchar_t *old_path = (wchar_t *)calloc(cap_chars, sizeof(wchar_t));
    wchar_t *new_path = (wchar_t *)calloc(cap_chars, sizeof(wchar_t));
    if (!old_path || !new_path) {
        free(old_path); free(new_path); RegCloseKey(key); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false;
    }

    DWORD read_bytes = (DWORD)(cap_chars * sizeof(wchar_t));
    rc = RegQueryValueExW(key, L"Path", NULL, &type, (LPBYTE)old_path, &read_bytes);
    if (rc == ERROR_FILE_NOT_FOUND) old_path[0] = L'\0';
    else if (rc != ERROR_SUCCESS) {
        free(old_path); free(new_path); RegCloseKey(key); SetLastError((DWORD)rc); return false;
    }
    old_path[cap_chars - 1] = L'\0';

    bool contains = path_list_contains(old_path, dir);
    if (add) {
        if (contains) {
            free(old_path); free(new_path); RegCloseKey(key); return true;
        }
        if (old_path[0]) {
            size_t len = wcslen(old_path);
            while (len && old_path[len - 1] == L';') old_path[--len] = L'\0';
            _snwprintf(new_path, cap_chars, L"%ls;%ls", old_path, dir);
        } else {
            wcsncpy(new_path, dir, cap_chars - 1);
        }
    } else {
        if (!contains) {
            free(old_path); free(new_path); RegCloseKey(key); return true;
        }
        const wchar_t *start = old_path;
        bool first = true;
        size_t used = 0;
        for (const wchar_t *p = old_path; ; ++p) {
            if (*p == L';' || *p == L'\0') {
                size_t token_len = (size_t)(p - start);
                if (!path_token_equal(start, token_len, dir) && token_len) {
                    if (!first && used + 1 < cap_chars) new_path[used++] = L';';
                    if (used + token_len < cap_chars) {
                        wmemcpy(new_path + used, start, token_len);
                        used += token_len;
                        new_path[used] = L'\0';
                    }
                    first = false;
                }
                if (*p == L'\0') break;
                start = p + 1;
            }
        }
    }

    size_t out_bytes = (wcslen(new_path) + 1) * sizeof(wchar_t);
    rc = RegSetValueExW(key, L"Path", 0, type, (const BYTE *)new_path, (DWORD)out_bytes);
    free(old_path); free(new_path); RegCloseKey(key);
    if (rc != ERROR_SUCCESS) { SetLastError((DWORD)rc); return false; }
    if (changed) *changed = true;
    broadcast_environment_change();
    return true;
}

static bool install_cli_binary(wchar_t *installed_exe, size_t cap, bool *path_changed) {
    wchar_t install_dir[MAX_PATH_WFT];
    if (!get_install_paths(install_dir, COUNTOF(install_dir), installed_exe, cap)) return false;
    if (!ensure_dir_recursive(install_dir)) return false;

    wchar_t current_exe[MAX_PATH_WFT];
    DWORD n = GetModuleFileNameW(NULL, current_exe, (DWORD)COUNTOF(current_exe));
    if (n == 0 || n >= COUNTOF(current_exe)) return false;

    if (_wcsicmp(current_exe, installed_exe) != 0) {
        if (!CopyFileW(current_exe, installed_exe, FALSE)) return false;
    }

    if (!update_machine_path(install_dir, true, path_changed)) return false;
    return true;
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
    wprintf(L"Usage:\n");
    wprintf(L"  winfreezetrace                       Run interactively\n");
    wprintf(L"  winfreezetrace run [options]         Run interactively\n");
    wprintf(L"  winfreezetrace install [options]     Install global CLI + auto-start service\n");
    wprintf(L"  winfreezetrace start                 Start installed service\n");
    wprintf(L"  winfreezetrace stop                  Stop installed service\n");
    wprintf(L"  winfreezetrace restart               Restart installed service\n");
    wprintf(L"  winfreezetrace status                Show service status\n");
    wprintf(L"  winfreezetrace uninstall             Remove service + global CLI PATH entry\n");
    wprintf(L"  winfreezetrace logs                  Print default service log path\n\n");
    wprintf(L"Monitor options:\n");
    wprintf(L"  --interval <ms>       Sample interval, default 500\n");
    wprintf(L"  --ring-minutes <n>    In-memory history, default 30\n");
    wprintf(L"  --pre-minutes <n>     Event history before trigger, default 5\n");
    wprintf(L"  --post-minutes <n>    Event capture after trigger, default 5\n");
    wprintf(L"  --latency-ms <n>      Trigger if disk read/write latency exceeds n, default 500\n");
    wprintf(L"  --queue <n>           Trigger if disk queue exceeds n, default 10\n");
    wprintf(L"  --log-dir <path>      Log directory\n");
    wprintf(L"  --no-process-io       Disable top process I/O sampling\n");
    wprintf(L"  --quiet               Reduce console output\n");
    wprintf(L"  --help                Show this help\n\n");
    wprintf(L"Service mode default logs: %%ProgramData%%\\WinFreezeTrace\\logs\n");
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

static bool parse_args(int argc, wchar_t **argv, int start_index, bool service_mode) {
    g_cfg.interval_ms = 500;
    g_cfg.ring_minutes = 30;
    g_cfg.pre_minutes = 5;
    g_cfg.post_minutes = 5;
    g_cfg.latency_trigger_ms = 500.0;
    g_cfg.queue_trigger = 10.0;
    g_cfg.process_io = true;
    g_cfg.quiet = false;
    default_log_dir(g_cfg.log_dir, COUNTOF(g_cfg.log_dir), service_mode);

    for (int i = start_index; i < argc; ++i) {
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

    if (service_mode) {
        if (!make_absolute_path(g_cfg.log_dir, COUNTOF(g_cfg.log_dir))) {
            fwprintf(stderr, L"Cannot resolve service log path.\n");
            return false;
        }
        g_cfg.quiet = true;
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

static int run_monitor(int argc, wchar_t **argv, int start_index, bool service_mode) {
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange64(&g_heartbeat_ms, 0);
    InterlockedExchange(&g_stage, STAGE_STARTING);
    InterlockedExchange(&g_watchdog_trigger_pending, 0);
    InterlockedExchange64(&g_watchdog_gap_ms, 0);

    if (!parse_args(argc, argv, start_index, service_mode)) {
        return 2;
    }

    InitializeCriticalSection(&g_log_lock);
    if (!service_mode) SetConsoleCtrlHandler(console_handler, TRUE);
    if (!ensure_dir_recursive(g_cfg.log_dir)) {
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
        g_ring = NULL;
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

        Sample sample;
        ZeroMemory(&sample, sizeof(sample));
        sample.seq = ++seq;
        sample.mono_ms = mono_ms();
        GetSystemTimeAsFileTime(&sample.utc);
        sample.interval_ms = (double)(sample.mono_ms - last_sample_ms);
        last_sample_ms = sample.mono_ms;

        uint64_t t0 = mono_ms();
        heartbeat(STAGE_PDH_COLLECT);
        PDH_STATUS st = PdhCollectQueryData(pdh.query);
        uint64_t t1 = mono_ms();
        sample.pdh_ms = (double)(t1 - t0);
        if (st != ERROR_SUCCESS) {
            wchar_t line[256], ts[64];
            filetime_to_local_text(sample.utc, ts, COUNTOF(ts));
            _snwprintf(line, COUNTOF(line), L"%ls PdhCollectQueryData failed 0x%08lx", ts, (unsigned long)st);
            log_line_locked(L"errors.log", line);
        }

        heartbeat(STAGE_PDH_FORMAT);
        sample.cpu_pct = pdh_value(pdh.cpu);
        sample.dpc_pct = pdh_value(pdh.dpc);
        sample.interrupt_pct = pdh_value(pdh.interrupt);
        sample.avail_mb = pdh_value(pdh.avail_mb);
        sample.pages_sec = pdh_value(pdh.pages_sec);
        sample.process_count = pdh_value(pdh.processes);
        sample.thread_count = pdh_value(pdh.threads);
        sample.uptime_sec = pdh_value(pdh.uptime);
        collect_disk_arrays(&pdh, &sample);

        uint64_t p0 = mono_ms();
        heartbeat(STAGE_PROCESS_SCAN);
        collect_process_io(&sample);
        sample.proc_ms = (double)(mono_ms() - p0);

        sample.trigger_flags = evaluate_triggers(&sample);

        heartbeat(STAGE_RING_UPDATE);
        ring_push(&sample);

        if (sample.trigger_flags) start_event_capture(&sample, sample.trigger_flags);

        heartbeat(STAGE_FILE_WRITE);
        rolling_write(&sample);
        event_append(&sample);
        print_status(&sample);

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
    g_ring = NULL;
    g_ring_cap = g_ring_count = g_ring_head = 0;
    DeleteCriticalSection(&g_log_lock);
    return 0;
}

static const wchar_t *service_state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return L"STOPPED";
        case SERVICE_START_PENDING: return L"START_PENDING";
        case SERVICE_STOP_PENDING: return L"STOP_PENDING";
        case SERVICE_RUNNING: return L"RUNNING";
        case SERVICE_CONTINUE_PENDING: return L"CONTINUE_PENDING";
        case SERVICE_PAUSE_PENDING: return L"PAUSE_PENDING";
        case SERVICE_PAUSED: return L"PAUSED";
        default: return L"UNKNOWN";
    }
}

static void service_set_status(DWORD state, DWORD win32_exit, DWORD wait_hint) {
    ZeroMemory(&g_service_status, sizeof(g_service_status));
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    if (g_service_status_handle) SetServiceStatus(g_service_status_handle, &g_service_status);
}

static void WINAPI service_ctrl_handler(DWORD control) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            InterlockedExchange(&g_stop, 1);
            service_set_status(SERVICE_STOP_PENDING, NO_ERROR, 20000);
            break;
        case SERVICE_CONTROL_INTERROGATE:
        default:
            if (g_service_status_handle) SetServiceStatus(g_service_status_handle, &g_service_status);
            break;
    }
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
    (void)argc;
    (void)argv;
    g_service_status_handle = RegisterServiceCtrlHandlerW(SERVICE_NAME, service_ctrl_handler);
    if (!g_service_status_handle) return;

    service_set_status(SERVICE_START_PENDING, NO_ERROR, 5000);
    service_set_status(SERVICE_RUNNING, NO_ERROR, 0);
    int rc = run_monitor(g_main_argc, g_main_argv, 2, true);

    if (rc == 0) {
        service_set_status(SERVICE_STOPPED, NO_ERROR, 0);
    } else {
        ZeroMemory(&g_service_status, sizeof(g_service_status));
        g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        g_service_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_service_status.dwServiceSpecificExitCode = (DWORD)rc;
        if (g_service_status_handle) SetServiceStatus(g_service_status_handle, &g_service_status);
    }
}

static bool query_service_status_process(SC_HANDLE svc, SERVICE_STATUS_PROCESS *ssp) {
    DWORD needed = 0;
    ZeroMemory(ssp, sizeof(*ssp));
    return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)ssp, sizeof(*ssp), &needed) != FALSE;
}

static bool wait_for_service_state(SC_HANDLE svc, DWORD target, DWORD timeout_ms) {
    uint64_t deadline = mono_ms() + timeout_ms;
    SERVICE_STATUS_PROCESS ssp;
    while (mono_ms() < deadline) {
        if (!query_service_status_process(svc, &ssp)) return false;
        if (ssp.dwCurrentState == target) return true;
        if (target == SERVICE_RUNNING && ssp.dwCurrentState == SERVICE_STOPPED) return false;
        Sleep(200);
    }
    return false;
}

static void print_scm_error(const wchar_t *action, DWORD err) {
    if (err == ERROR_ACCESS_DENIED) {
        fwprintf(stderr, L"%ls failed: access denied. Open CMD/PowerShell as Administrator.\n", action);
    } else if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
        fwprintf(stderr, L"%ls failed: WinFreezeTrace service is not installed.\n", action);
    } else {
        fwprintf(stderr, L"%ls failed (Windows error %lu).\n", action, (unsigned long)err);
    }
}

static int command_install(int argc, wchar_t **argv) {
    if (!parse_args(argc, argv, 2, true)) return 2;

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        print_scm_error(L"Install", GetLastError());
        return 4;
    }

    bool updated = false;
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME,
        SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
    if (svc) {
        updated = true;
        SERVICE_STATUS_PROCESS existing_status;
        if (query_service_status_process(svc, &existing_status) &&
            existing_status.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS stop_status;
            if (!ControlService(svc, SERVICE_CONTROL_STOP, &stop_status)) {
                DWORD stop_err = GetLastError();
                if (stop_err != ERROR_SERVICE_NOT_ACTIVE) {
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    print_scm_error(L"Stop old service before update", stop_err);
                    return 5;
                }
            }
            if (!wait_for_service_state(svc, SERVICE_STOPPED, 30000)) {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                fwprintf(stderr, L"Existing service did not stop; update was not applied.\n");
                return 5;
            }
        }
    } else {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(scm);
            print_scm_error(L"Open service", err);
            return 5;
        }
    }

    /* Copy only after the old service has stopped, so an in-use image cannot block an update. */
    wchar_t exe[MAX_PATH_WFT];
    bool path_changed = false;
    if (!install_cli_binary(exe, COUNTOF(exe), &path_changed)) {
        DWORD err = GetLastError();
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err == ERROR_ACCESS_DENIED)
            fwprintf(stderr, L"Install failed: access denied. Open CMD/PowerShell as Administrator.\n");
        else
            fwprintf(stderr, L"Install failed while copying CLI/updating PATH (Windows error %lu).\n", (unsigned long)err);
        return 3;
    }

    wchar_t bin_path[4096];
    _snwprintf(bin_path, COUNTOF(bin_path),
        L"\"%ls\" service-run --quiet --interval %lu --ring-minutes %lu --pre-minutes %lu "
        L"--post-minutes %lu --latency-ms %.3f --queue %.3f --log-dir \"%ls\"%ls",
        exe,
        (unsigned long)g_cfg.interval_ms,
        (unsigned long)g_cfg.ring_minutes,
        (unsigned long)g_cfg.pre_minutes,
        (unsigned long)g_cfg.post_minutes,
        g_cfg.latency_trigger_ms,
        g_cfg.queue_trigger,
        g_cfg.log_dir,
        g_cfg.process_io ? L"" : L" --no-process-io");
    bin_path[COUNTOF(bin_path) - 1] = L'\0';

    if (svc) {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  bin_path, NULL, NULL, NULL, NULL, NULL, SERVICE_DISPLAY_NAME)) {
            DWORD err = GetLastError();
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            print_scm_error(L"Update service", err);
            return 5;
        }
    } else {
        svc = CreateServiceW(
            scm, SERVICE_NAME, SERVICE_DISPLAY_NAME,
            SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            bin_path, NULL, NULL, NULL, NULL, NULL);
        if (!svc) {
            DWORD err = GetLastError();
            CloseServiceHandle(scm);
            print_scm_error(L"Create service", err);
            return 6;
        }
    }

    SERVICE_DESCRIPTIONW desc;
    desc.lpDescription = L"Low-overhead black-box recorder for Windows freezes, disk stalls, DPC/interrupt spikes and process I/O.";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_RESTART, 30000 }
    };
    SERVICE_FAILURE_ACTIONSW failure;
    ZeroMemory(&failure, sizeof(failure));
    failure.dwResetPeriod = 86400;
    failure.cActions = (DWORD)COUNTOF(actions);
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            print_scm_error(L"Start service", err);
            return 7;
        }
    }

    bool running = wait_for_service_state(svc, SERVICE_RUNNING, 15000);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    wprintf(L"WinFreezeTrace service %ls.\n", updated ? L"updated" : L"installed");
    wprintf(L"Startup: Automatic (starts at Windows boot)\n");
    wprintf(L"State: %ls\n", running ? L"RUNNING" : L"start requested; check `winfreezetrace status`");
    wprintf(L"Logs: %ls\n", g_cfg.log_dir);
    wprintf(L"CLI: %ls\n", exe);
    wprintf(L"PATH: %ls\n", path_changed ? L"added to Machine PATH" : L"already configured");
    wprintf(L"Open a NEW CMD/PowerShell window, then use `winfreezetrace` from anywhere.\n");
    return 0;
}

static int command_start(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { print_scm_error(L"Start", GetLastError()); return 2; }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { DWORD e = GetLastError(); CloseServiceHandle(scm); print_scm_error(L"Start", e); return 3; }

    SERVICE_STATUS_PROCESS ssp;
    if (query_service_status_process(svc, &ssp) && ssp.dwCurrentState == SERVICE_RUNNING) {
        wprintf(L"WinFreezeTrace is already RUNNING (PID %lu).\n", (unsigned long)ssp.dwProcessId);
        CloseServiceHandle(svc); CloseServiceHandle(scm); return 0;
    }

    if (!StartServiceW(svc, 0, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(svc); CloseServiceHandle(scm); print_scm_error(L"Start", e); return 4;
        }
    }
    bool ok = wait_for_service_state(svc, SERVICE_RUNNING, 15000);
    if (ok && query_service_status_process(svc, &ssp))
        wprintf(L"WinFreezeTrace RUNNING (PID %lu).\n", (unsigned long)ssp.dwProcessId);
    else
        fwprintf(stderr, L"Service did not reach RUNNING state within the wait window.\n");
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return ok ? 0 : 5;
}

static int command_stop(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { print_scm_error(L"Stop", GetLastError()); return 2; }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { DWORD e = GetLastError(); CloseServiceHandle(scm); print_scm_error(L"Stop", e); return 3; }

    SERVICE_STATUS_PROCESS ssp;
    if (query_service_status_process(svc, &ssp) && ssp.dwCurrentState == SERVICE_STOPPED) {
        wprintf(L"WinFreezeTrace is already STOPPED.\n");
        CloseServiceHandle(svc); CloseServiceHandle(scm); return 0;
    }

    SERVICE_STATUS status;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_NOT_ACTIVE) {
            CloseServiceHandle(svc); CloseServiceHandle(scm); print_scm_error(L"Stop", e); return 4;
        }
    }
    bool ok = wait_for_service_state(svc, SERVICE_STOPPED, 30000);
    if (ok) wprintf(L"WinFreezeTrace STOPPED.\n");
    else fwprintf(stderr, L"Service did not stop within the wait window.\n");
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return ok ? 0 : 5;
}

static int command_status(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { print_scm_error(L"Status", GetLastError()); return 2; }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
            wprintf(L"WinFreezeTrace service: NOT INSTALLED\n");
            return 1;
        }
        print_scm_error(L"Status", e); return 3;
    }

    SERVICE_STATUS_PROCESS ssp;
    if (!query_service_status_process(svc, &ssp)) {
        DWORD e = GetLastError(); CloseServiceHandle(svc); CloseServiceHandle(scm); print_scm_error(L"Status", e); return 4;
    }

    DWORD needed = 0;
    QueryServiceConfigW(svc, NULL, 0, &needed);
    QUERY_SERVICE_CONFIGW *cfg = needed ? (QUERY_SERVICE_CONFIGW *)malloc(needed) : NULL;
    if (cfg && QueryServiceConfigW(svc, cfg, needed, &needed)) {
        const wchar_t *startup = L"Other";
        if (cfg->dwStartType == SERVICE_AUTO_START) startup = L"Automatic";
        else if (cfg->dwStartType == SERVICE_DEMAND_START) startup = L"Manual";
        else if (cfg->dwStartType == SERVICE_DISABLED) startup = L"Disabled";
        wprintf(L"Service: WinFreezeTrace\nState: %ls\nPID: %lu\nStartup: %ls\nBinary: %ls\n",
                service_state_name(ssp.dwCurrentState), (unsigned long)ssp.dwProcessId,
                startup, cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"-");
    } else {
        wprintf(L"Service: WinFreezeTrace\nState: %ls\nPID: %lu\n",
                service_state_name(ssp.dwCurrentState), (unsigned long)ssp.dwProcessId);
    }
    free(cfg);
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return 0;
}

static int command_uninstall(void) {
    bool service_removed = false;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { print_scm_error(L"Uninstall", GetLastError()); return 2; }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (svc) {
        SERVICE_STATUS_PROCESS ssp;
        if (query_service_status_process(svc, &ssp) && ssp.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS status;
            ControlService(svc, SERVICE_CONTROL_STOP, &status);
            wait_for_service_state(svc, SERVICE_STOPPED, 30000);
        }

        if (!DeleteService(svc)) {
            DWORD e = GetLastError();
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            print_scm_error(L"Delete service", e);
            return 4;
        }
        service_removed = true;
        CloseServiceHandle(svc);
    } else {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(scm);
            print_scm_error(L"Uninstall", e);
            return 3;
        }
    }
    CloseServiceHandle(scm);

    wchar_t install_dir[MAX_PATH_WFT], installed_exe[MAX_PATH_WFT];
    get_install_paths(install_dir, COUNTOF(install_dir), installed_exe, COUNTOF(installed_exe));
    bool path_changed = false;
    if (!update_machine_path(install_dir, false, &path_changed)) {
        fwprintf(stderr, L"Warning: Machine PATH cleanup failed (Windows error %lu).\n",
                 (unsigned long)GetLastError());
    }

    wchar_t current_exe[MAX_PATH_WFT];
    DWORD n = GetModuleFileNameW(NULL, current_exe, (DWORD)COUNTOF(current_exe));
    bool running_from_installed = n > 0 && n < COUNTOF(current_exe) &&
                                  _wcsicmp(current_exe, installed_exe) == 0;
    if (running_from_installed) {
        if (!MoveFileExW(installed_exe, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            fwprintf(stderr, L"Warning: CLI binary could not be scheduled for deletion (Windows error %lu).\n",
                     (unsigned long)GetLastError());
        } else {
            /* Directory deletion is queued after the executable deletion. */
            MoveFileExW(install_dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } else {
        DeleteFileW(installed_exe);
        RemoveDirectoryW(install_dir);
    }

    wprintf(L"WinFreezeTrace service: %ls.\n", service_removed ? L"removed" : L"already absent");
    wprintf(L"Global CLI PATH entry: %ls.\n", path_changed ? L"removed" : L"already absent");
    if (running_from_installed)
        wprintf(L"Installed CLI binary will be deleted at the next reboot because it is currently running.\n");
    wprintf(L"Logs were kept. Open a NEW terminal for PATH removal to be reflected there.\n");
    return 0;
}

static int command_logs(void) {
    wchar_t path[MAX_PATH_WFT];
    default_log_dir(path, COUNTOF(path), true);
    make_absolute_path(path, COUNTOF(path));
    wprintf(L"%ls\n", path);
    return 0;
}

static int command_service_run(void) {
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SERVICE_NAME, service_main },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD e = GetLastError();
        if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fwprintf(stderr, L"`service-run` is internal and must be launched by Windows Service Control Manager.\n");
        } else {
            fwprintf(stderr, L"StartServiceCtrlDispatcher failed (Windows error %lu).\n", (unsigned long)e);
        }
        return 8;
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    g_main_argc = argc;
    g_main_argv = argv;

    if (argc <= 1) return run_monitor(argc, argv, 1, false);

    const wchar_t *cmd = argv[1];
    if (!wcscmp(cmd, L"--help") || !wcscmp(cmd, L"-h") || !wcscmp(cmd, L"help")) {
        print_usage();
        return 0;
    }
    if (!wcscmp(cmd, L"run")) return run_monitor(argc, argv, 2, false);
    if (!wcscmp(cmd, L"install")) return command_install(argc, argv);
    if (!wcscmp(cmd, L"start")) return command_start();
    if (!wcscmp(cmd, L"stop")) return command_stop();
    if (!wcscmp(cmd, L"restart")) {
        int rc = command_stop();
        if (rc != 0 && rc != 5) return rc;
        return command_start();
    }
    if (!wcscmp(cmd, L"status")) return command_status();
    if (!wcscmp(cmd, L"uninstall")) return command_uninstall();
    if (!wcscmp(cmd, L"logs")) return command_logs();
    if (!wcscmp(cmd, L"service-run")) return command_service_run();

    /* Backward compatibility: options without `run` still start interactively. */
    if (cmd[0] == L'-') return run_monitor(argc, argv, 1, false);

    fwprintf(stderr, L"Unknown command: %ls\n\n", cmd);
    print_usage();
    return 2;
}
