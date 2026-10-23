/* metrics.h - PDH counters + memory/system info sampled once per interval. */
#ifndef WFT_METRICS_H
#define WFT_METRICS_H
#include "platform.h"

typedef struct ArrayCounter {
    PDH_HCOUNTER h;
    BYTE  *buf;
    DWORD  cap;
} ArrayCounter;

#define WFT_PDH_SINGLES 20
#define WFT_PDH_ARRAYS  5

typedef struct PdhState {
    PDH_HQUERY   q;
    PDH_HCOUNTER single[WFT_PDH_SINGLES];
    ArrayCounter arr[WFT_PDH_ARRAYS];
    bool         ok;
    int          missing;      /* counters that could not be added */
} PdhState;

bool metrics_init(PdhState *p);
void metrics_shutdown(PdhState *p);
void metrics_collect(PdhState *p, Metrics *m);

/* One-shot read of a wildcard counter (thermal zones etc). Returns false if unavailable. */
bool pdh_oneshot_extreme(const WCHAR *path, bool want_max, double *value, char *name, size_t name_cch);

#endif
