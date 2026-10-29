#include "dump.h"

const char *dump_result_text(DumpResult r) {
    switch (r) {
        case DUMP_OK: return "written";
        case DUMP_DISABLED: return "disabled (--no-dump)";
        case DUMP_NO_TARGET: return "no foreground process";
        case DUMP_SELF: return "skipped: target is WinFreezeTrace itself";
        case DUMP_DENYLISTED: return "skipped: credential/system-critical process is never dumped";
        case DUMP_LOW_DISK: return "skipped: free disk space below --min-free-mb";
        case DUMP_NO_ACCESS: return "not available: access denied (protected or elevated process)";
        case DUMP_SKIPPED_STORAGE: return "skipped: storage stall (a dump would suspend the foreground app and write to the slow disk)";
        case DUMP_TIMEOUT: return "aborted: exceeded --dump-timeout-ms (partial file removed)";
        default: return "failed";
    }
}

/* Processes that must never be dumped: credential stores and session-critical system processes. */
static bool denylisted(const WCHAR *exe_path) {
    static const WCHAR *const deny[] = { L"lsass.exe", L"lsaiso.exe", L"csrss.exe", L"winlogon.exe", L"wininit.exe",
                                         L"services.exe", L"smss.exe", L"system", L"securekernel.exe" };
    const WCHAR *b = wft_base_name(exe_path);
    for (size_t i = 0; i < sizeof deny / sizeof deny[0]; ++i) if (_wcsicmp(b, deny[i]) == 0) return true;
    return false;
}

typedef struct DumpCtx { ULONGLONG deadline; BOOL timed_out; } DumpCtx;

static BOOL CALLBACK dump_cb(PVOID param, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    DumpCtx *c = (DumpCtx *)param;
    if (in->CallbackType == CancelCallback) {
        out->CheckCancel = TRUE;
        if (GetTickCount64() > c->deadline) { out->Cancel = TRUE; c->timed_out = TRUE; }
        return TRUE;
    }
    return TRUE;
}

DumpResult dump_write(const WCHAR *dir, DWORD pid, const WCHAR *exe_path, const Config *cfg, DWORD *last_error, double *elapsed_ms) {
    *last_error = 0; *elapsed_ms = 0;
    if (!cfg->make_dump) return DUMP_DISABLED;
    if (!pid) return DUMP_NO_TARGET;
    if (pid == GetCurrentProcessId()) return DUMP_SELF;
    if (denylisted(exe_path)) return DUMP_DENYLISTED;
    uint64_t free_mb;
    if (cfg->min_free_mb && plat_free_mb(dir, &free_mb) && free_mb < cfg->min_free_mb) return DUMP_LOW_DISK;

    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);   /* the minimum MiniDumpWriteDump needs */
    if (!hp) { *last_error = GetLastError(); return DUMP_NO_ACCESS; }
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\target.dmp", dir);
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);   /* no sharing, never overwrite */
    if (hf == INVALID_HANDLE_VALUE) { *last_error = GetLastError(); CloseHandle(hp); return DUMP_FAILED; }

    DumpCtx ctx; ctx.deadline = GetTickCount64() + cfg->dump_timeout_ms; ctx.timed_out = FALSE;
    MINIDUMP_CALLBACK_INFORMATION cbi; cbi.CallbackRoutine = dump_cb; cbi.CallbackParam = &ctx;
    MINIDUMP_TYPE mt = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    double t0 = plat_qpc_ms();
    BOOL ok = MiniDumpWriteDump(hp, pid, hf, mt, NULL, NULL, &cbi);   /* suspends the target while it runs */
    *elapsed_ms = plat_qpc_ms() - t0;
    if (!ok) *last_error = GetLastError();
    CloseHandle(hf); CloseHandle(hp);
    if (ok) return DUMP_OK;
    DeleteFileW(path);
    return ctx.timed_out ? DUMP_TIMEOUT : DUMP_FAILED;
}
