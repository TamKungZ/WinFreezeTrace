/* foreground.h - foreground window identity, responsiveness probe, details report. */
#ifndef WFT_FOREGROUND_H
#define WFT_FOREGROUND_H
#include "platform.h"

typedef struct ForegroundInfo {
    HWND   hwnd;
    DWORD  pid, tid;
    int    status;                 /* ProbeStatus */
    double probe_ms;               /* WFT_NA if not measured */
    WCHAR  path[WFT_PATH_CCH];
    WCHAR  title[512];
    WCHAR  cls[128];
} ForegroundInfo;

void foreground_probe(ForegroundInfo *fi, DWORD timeout_ms, DWORD self_pid);
void foreground_to_sample(const ForegroundInfo *fi, Sample *s);
const char *probe_status_name(int status);
/* Writes foreground.txt (identity, elevation, bitness, uptime, window geometry, memory, I/O, idle time). */
void foreground_write_details(const WCHAR *dir, const ForegroundInfo *fi, const Config *cfg);

#endif
