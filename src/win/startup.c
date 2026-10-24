#include "startup.h"

#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define VALUE_NAME   L"WinFreezeTrace"

/* Task Manager / Settings record "disabled" as a binary value whose first byte is odd (0x03); 0x02/0x06 mean enabled. */
static bool disabled_by_user(void) {
    BYTE data[16] = {0}; DWORD sz = sizeof data, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, VALUE_NAME, RRF_RT_REG_BINARY, &type, data, &sz) != ERROR_SUCCESS) return false;
    return sz >= 1 && (data[0] & 1) != 0;
}

bool startup_is_enabled(void) {
    WCHAR v[2048]; DWORD sz = sizeof v, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, v, &sz) != ERROR_SUCCESS) return false;
    return v[0] != 0 && !disabled_by_user();
}

bool startup_set(bool enable, const WCHAR *args) {
    if (!enable) {
        LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, VALUE_NAME);          /* stale flag: ignore result */
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    WCHAR exe[WFT_PATH_CCH];
    if (!plat_exe_path(exe, WFT_PATH_CCH)) return false;
    /* The program path is always quoted (a path may contain spaces; quoting a plain one is harmless). */
    WCHAR cmd[WFT_PATH_CCH + 2400];
    if (args && args[0]) wft_wsnprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" %ls", exe, args);
    else                 wft_wsnprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\"", exe);
    DWORD bytes = (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR));
    LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, VALUE_NAME, REG_SZ, cmd, bytes);
    if (r != ERROR_SUCCESS) return false;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, VALUE_NAME);              /* re-enable if it had been switched off */
    return true;
}
