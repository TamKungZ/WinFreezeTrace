/* dump.h - bounded, least-privilege minidump of the foreground process. */
#ifndef WFT_DUMP_H
#define WFT_DUMP_H
#include "platform.h"

typedef enum DumpResult {
    DUMP_OK = 0, DUMP_DISABLED, DUMP_NO_TARGET, DUMP_SELF, DUMP_DENYLISTED, DUMP_LOW_DISK,
    DUMP_NO_ACCESS, DUMP_TIMEOUT, DUMP_FAILED
} DumpResult;

DumpResult  dump_write(const WCHAR *dir, DWORD pid, const WCHAR *exe_path, const Config *cfg, DWORD *last_error, double *elapsed_ms);
const char *dump_result_text(DumpResult r);

#endif
