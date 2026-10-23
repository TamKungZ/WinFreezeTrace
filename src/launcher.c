#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define CORE_DLL L"WinFreezeTraceCore.dll"

typedef int (*WftRun_t)(HINSTANCE);
typedef BOOL (WINAPI *SetDefaultDllDirectories_t)(DWORD);

static void fail(const WCHAR *msg, DWORD err) {
    WCHAR buf[600];
    _snwprintf(buf, 600, L"%ls\n(error %lu)", msg, (unsigned long)err);
    buf[599] = 0;
    MessageBoxW(NULL, buf, L"WinFreezeTrace", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline; (void)show;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        SetDefaultDllDirectories_t fn = (SetDefaultDllDirectories_t)(void *)GetProcAddress(k32, "SetDefaultDllDirectories");
        if (fn) fn(0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    }

    WCHAR path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { fail(L"Cannot determine the program folder.", GetLastError()); return 1; }
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!slash) { fail(L"Cannot determine the program folder.", 0); return 1; }
    wcscpy(slash + 1, CORE_DLL);

    HMODULE core = LoadLibraryW(path);
    if (!core) { fail(L"Cannot load " CORE_DLL L" from the program folder. Reinstall WinFreezeTrace.", GetLastError()); return 1; }
    WftRun_t run = (WftRun_t)(void *)GetProcAddress(core, "WftRun");
    if (!run) { fail(CORE_DLL L" is missing the WftRun entry point (wrong version?).", GetLastError()); return 1; }
    return run(inst);
}
