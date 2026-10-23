/* strutil.h - safe formatting, UTF-8, CSV/JSON output and parsing helpers. */
#ifndef WFT_STRUTIL_H
#define WFT_STRUTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <wchar.h>

/* printf-style formatting that ALWAYS NUL-terminates (unlike _snwprintf). */
int wft_snprintf(char *dst, size_t cch, const char *fmt, ...);
int wft_wsnprintf(wchar_t *dst, size_t cch, const wchar_t *fmt, ...);

/* Strict unsigned decimal: digits only, no sign/space/overflow. */
bool wft_parse_u32(const wchar_t *s, uint32_t *out);

/* UTF-16/UTF-32 -> UTF-8. Lone surrogates become U+FFFD. Always terminates when cap>0.
 * Returns bytes written (excluding NUL). Truncates on a character boundary. */
size_t wft_wcs_to_utf8(const wchar_t *s, char *dst, size_t cap);

/* Plain UTF-8 output (no quoting). */
void wft_write_utf8(FILE *f, const wchar_t *s);

/* RFC 4180 CSV field. Control characters become spaces and a leading
 * = + - @ is neutralised with a quote so spreadsheets will not execute it. */
void wft_csv_text(FILE *f, const wchar_t *s);
void wft_csv_utf8(FILE *f, const char *s);

/* JSON string literal (adds the quotes). */
void wft_json_str(FILE *f, const char *utf8);
void wft_json_wstr(FILE *f, const wchar_t *s);

const wchar_t *wft_base_name(const wchar_t *path);
bool wft_wcs_istarts(const wchar_t *s, const wchar_t *prefix);
bool wft_wcs_icontains(const wchar_t *hay, const wchar_t *needle);
bool wft_wcs_ieq(const wchar_t *a, const wchar_t *b);

/* C:\Users\alice\x -> C:\Users\<user>\x (bounded). */
void wft_redact_user_path(wchar_t *path, size_t cch);

/* ASCII-only copy for device strings; non-ASCII becomes '?'. */
void wft_ascii_from_wide(char *dst, size_t cch, const wchar_t *src);

#endif
