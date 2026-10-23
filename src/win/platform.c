#include "platform.h"

static LARGE_INTEGER g_qpf;
static WCHAR g_log_path[WFT_PATH_CCH];
static CRITICAL_SECTION g_log_cs;
static bool g_log_ready = false;
static PSECURITY_DESCRIPTOR g_private_sd = NULL;

void plat_init(void) { QueryPerformanceFrequency(&g_qpf); if (g_qpf.QuadPart == 0) g_qpf.QuadPart = 1; }

double plat_qpc_ms(void) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

void plat_local_time(WftTime *t) {
    SYSTEMTIME st; GetLocalTime(&st);
    t->year = st.wYear; t->month = st.wMonth; t->day = st.wDay;
    t->hour = st.wHour; t->minute = st.wMinute; t->second = st.wSecond; t->millis = st.wMilliseconds;
}

DWORD plat_self_pid(void) { return GetCurrentProcessId(); }

typedef BOOL (WINAPI *SetDefaultDllDirectories_t)(DWORD);

void plat_harden_process(void) {
    /* Only load DLLs from System32 (blocks DLL planting next to the exe / in the CWD). */
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        SetDefaultDllDirectories_t fn = (SetDefaultDllDirectories_t)(void *)GetProcAddress(k32, "SetDefaultDllDirectories");
        if (fn) fn(0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    }
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);   /* no "insert disk" popups while enumerating drives */
}

static bool build_private_sd(void) {
    if (g_private_sd) return true;
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, NULL, 0, &need);
    TOKEN_USER *tu = need ? (TOKEN_USER *)malloc(need) : NULL;
    bool ok = false;
    if (tu && GetTokenInformation(tok, TokenUser, tu, need, &need)) {
        LPWSTR sid = NULL;
        if (ConvertSidToStringSidW(tu->User.Sid, &sid)) {
            WCHAR sddl[256];
            /* Protected DACL: full control for this user and SYSTEM only, inherited by files. */
            wft_wsnprintf(sddl, 256, L"D:P(A;OICI;FA;;;%ls)(A;OICI;FA;;;SY)", sid);
            ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &g_private_sd, NULL) != 0;
            LocalFree(sid);
        }
    }
    free(tu);
    CloseHandle(tok);
    return ok;
}

DirResult plat_create_private_dir(const WCHAR *path) {
    SECURITY_ATTRIBUTES sa; SECURITY_ATTRIBUTES *psa = NULL;
    if (build_private_sd()) {
        sa.nLength = sizeof sa; sa.lpSecurityDescriptor = g_private_sd; sa.bInheritHandle = FALSE;
        psa = &sa;
    }
    if (CreateDirectoryW(path, psa)) return DIR_CREATED;
    return GetLastError() == ERROR_ALREADY_EXISTS ? DIR_EXISTS : DIR_FAILED;
}

void plat_default_paths(WCHAR *app_root, size_t app_cch, WCHAR *reports, size_t rep_cch) {
    WCHAR base[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, base)) || !base[0]) {
        DWORD n = GetTempPathW(MAX_PATH, base);       /* last resort; never the (often read-only) CWD */
        if (n == 0 || n >= MAX_PATH) wcscpy(base, L"C:\\");
        size_t l = wcslen(base);
        if (l && base[l - 1] == L'\\') base[l - 1] = 0;
    }
    wft_wsnprintf(app_root, app_cch, L"%ls\\WinFreezeTrace", base);
    wft_wsnprintf(reports, rep_cch, L"%ls\\reports", app_root);
    plat_create_private_dir(app_root);
    plat_create_private_dir(reports);
}

bool plat_abs_path(const WCHAR *in, WCHAR *out, size_t cch) {
    DWORD n = GetFullPathNameW(in, (DWORD)cch, out, NULL);
    return n > 0 && n < cch;
}

bool plat_free_mb(const WCHAR *path, uint64_t *mb) {
    ULARGE_INTEGER avail, total, freeb;
    if (!GetDiskFreeSpaceExW(path, &avail, &total, &freeb)) return false;
    *mb = avail.QuadPart / (1024ull * 1024ull);
    return true;
}

FILE *plat_fopen_w(const WCHAR *path) { return _wfopen(path, L"wb"); }

bool plat_pid_exe(DWORD pid, WCHAR *out, size_t cch) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD n = (DWORD)cch;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);
    if (!ok) out[0] = 0;
    return ok != 0;
}

void plat_print(const char *text) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        else return;                              /* started by Task Scheduler: nobody to show it to */
    }
    if (h && h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, text, (DWORD)strlen(text), &w, NULL); }
}

void log_init(const WCHAR *dir) {
    InitializeCriticalSection(&g_log_cs);
    wft_wsnprintf(g_log_path, WFT_PATH_CCH, L"%ls\\winfreezetrace.log", dir);
    g_log_ready = true;
}

void log_msg(const char *level, const char *fmt, ...) {
    if (!g_log_ready) return;
    char line[700], body[560];
    va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
    body[sizeof body - 1] = 0;
    WftTime t; plat_local_time(&t);
    wft_snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u [%s] %s\r\n", t.year, t.month, t.day, t.hour, t.minute, t.second, level, body);
    EnterCriticalSection(&g_log_cs);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 512 * 1024 && fad.nFileSizeHigh == 0) {
        WCHAR old[WFT_PATH_CCH]; wft_wsnprintf(old, WFT_PATH_CCH, L"%ls.old", g_log_path);
        MoveFileExW(g_log_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    FILE *f = _wfopen(g_log_path, L"ab");
    if (f) { fputs(line, f); fclose(f); }
    LeaveCriticalSection(&g_log_cs);
}

void log_msg_w(const char *level, const char *fmt, const WCHAR *w) {
    char u[1100];
    wft_wcs_to_utf8(w, u, sizeof u);
    log_msg(level, fmt, u);
}
