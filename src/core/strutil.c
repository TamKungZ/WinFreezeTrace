#include "strutil.h"
#include "wft_types.h"

#include <stdarg.h>
#include <string.h>
#include <wctype.h>

int wft_snprintf(char *dst, size_t cch, const char *fmt, ...) {
    if (!dst || cch == 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cch, fmt, ap);
    va_end(ap);
    dst[cch - 1] = 0;           /* old MSVCRT does not terminate on truncation */
    if (n < 0 || (size_t)n >= cch) return (int)strlen(dst);
    return n;
}

int wft_wsnprintf(wchar_t *dst, size_t cch, const wchar_t *fmt, ...) {
    if (!dst || cch == 0) return 0;
    va_list ap;
    va_start(ap, fmt);
#ifdef _WIN32
    int n = _vsnwprintf(dst, cch, fmt, ap);
#else
    int n = vswprintf(dst, cch, fmt, ap);
#endif
    va_end(ap);
    dst[cch - 1] = 0;
    if (n < 0 || (size_t)n >= cch) return (int)wcslen(dst);
    return n;
}

bool wft_parse_u32(const wchar_t *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const wchar_t *p = s; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        v = v * 10u + (uint64_t)(*p - L'0');
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = (uint32_t)v;
    return true;
}

size_t wft_wcs_to_utf8(const wchar_t *s, char *dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    size_t n = 0;
    if (!s) { dst[0] = 0; return 0; }
    for (size_t i = 0; s[i]; ++i) {
        uint32_t cp = (uint32_t)s[i];
#if WCHAR_MAX <= 0xFFFF
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = (uint32_t)s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u); ++i; }
            else cp = 0xFFFD;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
#else
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
#endif
        char t[4]; size_t len;
        if (cp < 0x80)        { t[0] = (char)cp; len = 1; }
        else if (cp < 0x800)  { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
        else if (cp < 0x10000){ t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
        else                  { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
        if (n + len >= cap) break;
        memcpy(dst + n, t, len);
        n += len;
    }
    dst[n] = 0;
    return n;
}

void wft_write_utf8(FILE *f, const wchar_t *s) {
    char buf[8192];
    size_t n = wft_wcs_to_utf8(s, buf, sizeof buf);
    if (n) fwrite(buf, 1, n, f);
}

void wft_csv_utf8(FILE *f, const char *s) {
    fputc('"', f);
    if (s && *s) {
        unsigned char c0 = (unsigned char)*s;
        if (c0 == '=' || c0 == '+' || c0 == '-' || c0 == '@' || c0 == '\t' || c0 == '\r')
            fputc('\'', f);
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            if (*p == '"') fputs("\"\"", f);
            else if (*p < 0x20 || *p == 0x7F) fputc(' ', f);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

void wft_csv_text(FILE *f, const wchar_t *s) {
    char buf[6144];
    wft_wcs_to_utf8(s, buf, sizeof buf);
    wft_csv_utf8(f, buf);
}

void wft_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

void wft_json_wstr(FILE *f, const wchar_t *s) {
    char buf[6144];
    wft_wcs_to_utf8(s, buf, sizeof buf);
    wft_json_str(f, buf);
}

const wchar_t *wft_base_name(const wchar_t *path) {
    const wchar_t *last = path;
    for (const wchar_t *p = path; p && *p; ++p)
        if (*p == L'\\' || *p == L'/') last = p + 1;
    return last ? last : L"";
}

static int wft_wcsnicmp_(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        wint_t x = towlower((wint_t)a[i]), y = towlower((wint_t)b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}

bool wft_wcs_istarts(const wchar_t *s, const wchar_t *prefix) {
    if (!s || !prefix) return false;
    size_t n = wcslen(prefix);
    if (wcslen(s) < n) return false;
    return wft_wcsnicmp_(s, prefix, n) == 0;
}

bool wft_wcs_ieq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return false;
    return wcslen(a) == wcslen(b) && wft_wcsnicmp_(a, b, wcslen(a)) == 0;
}

bool wft_wcs_icontains(const wchar_t *hay, const wchar_t *needle) {
    if (!hay || !needle) return false;
    size_t nl = wcslen(needle), hl = wcslen(hay);
    if (nl == 0) return true;
    if (hl < nl) return false;
    for (size_t i = 0; i + nl <= hl; ++i)
        if (wft_wcsnicmp_(hay + i, needle, nl) == 0) return true;
    return false;
}

void wft_redact_user_path(wchar_t *path, size_t cch) {
    static const wchar_t key[] = L"\\users\\";
    const size_t klen = 7;
    if (!path || cch < 16) return;
    size_t len = wcslen(path);
    for (size_t i = 0; i + klen < len; ++i) {
        if (wft_wcsnicmp_(path + i, key, klen) != 0) continue;
        size_t s = i + klen, e = s;
        while (e < len && path[e] != L'\\' && path[e] != L'/') ++e;
        if (e == s) return;
        wchar_t tmp[WFT_PATH_CCH + 16];
        size_t t = 0;
        for (size_t k = 0; k < s && t + 1 < sizeof tmp / sizeof tmp[0]; ++k) tmp[t++] = path[k];
        static const wchar_t rep[] = L"<user>";
        for (size_t k = 0; rep[k] && t + 1 < sizeof tmp / sizeof tmp[0]; ++k) tmp[t++] = rep[k];
        for (size_t k = e; k < len && t + 1 < sizeof tmp / sizeof tmp[0]; ++k) tmp[t++] = path[k];
        tmp[t] = 0;
        size_t copy = t < cch - 1 ? t : cch - 1;
        wmemcpy(path, tmp, copy);
        path[copy] = 0;
        return;
    }
}

void wft_ascii_from_wide(char *dst, size_t cch, const wchar_t *src) {
    if (!dst || cch == 0) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cch; ++i)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[i] = 0;
}

bool wft_quote_arg(const wchar_t *arg, wchar_t *dst, size_t cch) {
    if (!dst || cch == 0) return false;
    dst[0] = 0;
    if (!arg) arg = L"";
    bool plain = arg[0] != 0;
    for (const wchar_t *p = arg; *p && plain; ++p)
        if (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\v' || *p == L'"') plain = false;
    size_t n = 0;
#define PUT(ch) do { if (n + 1 >= cch) { dst[0] = 0; return false; } dst[n++] = (ch); } while (0)
    if (plain) {
        for (const wchar_t *p = arg; *p; ++p) PUT(*p);
        dst[n] = 0;
        return true;
    }
    PUT(L'"');
    for (const wchar_t *p = arg;; ++p) {
        size_t slashes = 0;
        while (*p == L'\\') { ++slashes; ++p; }
        if (*p == 0) {                         /* backslashes before the closing quote must be doubled */
            for (size_t k = 0; k < slashes * 2; ++k) PUT(L'\\');
            break;
        }
        if (*p == L'"') {                      /* backslashes before a literal quote are doubled, then the quote is escaped */
            for (size_t k = 0; k < slashes * 2 + 1; ++k) PUT(L'\\');
            PUT(L'"');
        } else {
            for (size_t k = 0; k < slashes; ++k) PUT(L'\\');
            PUT(*p);
        }
    }
    PUT(L'"');
#undef PUT
    dst[n] = 0;
    return true;
}
