#include "retention.h"
#include "strutil.h"

bool retention_dir_name_ok(const wchar_t *n) {
    if (!n) return false;
    for (int i = 0; i < 15; ++i) {
        if (i == 8) { if (n[i] != L'_') return false; }
        else if (n[i] < L'0' || n[i] > L'9') return false;
    }
    if (n[15] == 0) return true;
    if (n[15] != L'_') return false;
    for (const wchar_t *p = n + 16; *p; ++p) {
        bool ok = (*p >= L'0' && *p <= L'9') || (*p >= L'a' && *p <= L'z') || (*p >= L'A' && *p <= L'Z') || *p == L'_';
        if (!ok) return false;
    }
    return true;
}

bool retention_file_name_ok(const wchar_t *name) {
    static const wchar_t *const ext[] = { L".csv", L".txt", L".xml", L".dmp", L".json", L".log" };
    if (!name) return false;
    size_t len = wcslen(name);
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; ++i) {
        size_t el = wcslen(ext[i]);
        if (len > el && wft_wcs_ieq(name + len - el, ext[i])) return true;
    }
    return false;
}

int retention_select(const wchar_t *const *names, int n, int keep, int *out_idx) {
    if (keep <= 0 || n <= 0) return 0;
    int idx[4096], m = 0;
    for (int i = 0; i < n && m < 4096; ++i)
        if (retention_dir_name_ok(names[i])) idx[m++] = i;
    for (int i = 1; i < m; ++i) {                      /* insertion sort, oldest first */
        int v = idx[i], j = i - 1;
        while (j >= 0 && wcscmp(names[idx[j]], names[v]) > 0) { idx[j + 1] = idx[j]; --j; }
        idx[j + 1] = v;
    }
    int del = m - keep, c = 0;
    for (int i = 0; i < del; ++i) out_idx[c++] = idx[i];
    return c;
}
