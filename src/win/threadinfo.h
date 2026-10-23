/* threadinfo.h - foreground-process threads, wait chains and modules. */
#ifndef WFT_THREADINFO_H
#define WFT_THREADINFO_H
#include "platform.h"

typedef struct ThreadSnap {
    DWORD     *tids;
    LONG      *base_pri;
    ULONGLONG *cpu100ns;      /* first reading; 0 when the thread could not be opened */
    int        n;
    double     base_ms;       /* QPC time of the first reading */
    bool       truncated;
} ThreadSnap;

typedef struct HotThread { DWORD tid; double pct; char name[48]; bool gui; } HotThread;

typedef struct WaitInfo {
    bool  ok, any_cycle, budget_exhausted;
    int   scanned, in_chain;          /* threads examined / threads waiting on something */
    char  ui_blocker[200];            /* "" when the UI thread was not waiting on anything */
    bool  ui_cross_process;
    DWORD blocker_pid;
} WaitInfo;

void threads_begin(DWORD pid, ThreadSnap *ts);
/* Second reading; writes threads.csv; fills the hottest threads. Returns how many were found. */
int  threads_finish_write(const WCHAR *dir, DWORD pid, DWORD gui_tid, const ThreadSnap *ts, HotThread *top, int max_top);
void threads_free(ThreadSnap *ts);
void wait_chains_write(const WCHAR *dir, const ThreadSnap *ts, DWORD pid, DWORD gui_tid, DWORD budget_ms, WaitInfo *wi);
void modules_write(const WCHAR *dir, DWORD pid, const Config *cfg);

#endif
