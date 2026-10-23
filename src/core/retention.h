/* retention.h - choose which old incident folders to delete (pure logic; the
 * file-system work lives in win/report.c). */
#ifndef WFT_RETENTION_H
#define WFT_RETENTION_H
#include "wft_types.h"

/* True for "YYYYMMDD_HHMMSS" optionally followed by "_suffix" (letters, digits, '_'). */
bool retention_dir_name_ok(const wchar_t *name);
/* Only files with these extensions may be deleted: csv txt xml dmp json log. */
bool retention_file_name_ok(const wchar_t *name);
/* names[i] that match the incident pattern are sorted oldest-first; everything
 * beyond the newest `keep` is returned in out_idx. Returns the count. keep==0 => none. */
int  retention_select(const wchar_t *const *names, int n, int keep, int *out_idx);

#endif
