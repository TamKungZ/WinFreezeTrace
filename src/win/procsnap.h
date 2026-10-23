/* procsnap.h - two-point process snapshot (CPU/I-O/fault deltas) for processes.csv. */
#ifndef WFT_PROCSNAP_H
#define WFT_PROCSNAP_H
#include "platform.h"

typedef struct ProcBase {
    DWORD pid; ULONGLONG created, cpu100ns, read_bytes, write_bytes; DWORD faults;
} ProcBase;

typedef struct ProcRow {
    DWORD pid, ppid, session, threads, handles, page_faults;
    double cpu_system_pct, cpu_one_core_pct, read_bps, write_bps, faults_per_s, cpu_total_s, age_min;
    SIZE_T working_set, peak_working_set, private_bytes;
    ULONGLONG read_bytes, write_bytes, read_ops, write_ops;
    bool is_fg, no_access;
    WCHAR path[520];
} ProcRow;

typedef struct ProcSnap {
    ProcBase *base; int base_n; double base_ms; bool truncated;
    ProcRow *rows; int row_n;
} ProcSnap;

void procsnap_begin(ProcSnap *ps);
/* Takes the second reading; rows are sorted by CPU use (descending). */
void procsnap_finish(ProcSnap *ps, DWORD fg_pid);
bool procsnap_write_csv(const WCHAR *dir, const ProcSnap *ps, const Config *cfg);
void procsnap_free(ProcSnap *ps);

typedef struct TopProc { DWORD pid; char name[64]; double value; } TopProc;
/* kind 0 = CPU % of whole system, 1 = disk I/O bytes/s (read+write). */
int  procsnap_top(const ProcSnap *ps, int kind, TopProc *out, int max_out);

#endif
