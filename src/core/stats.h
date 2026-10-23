/* stats.h - window statistics over the sample timeline. */
#ifndef WFT_STATS_H
#define WFT_STATS_H
#include "wft_types.h"

typedef double (*MetricFn)(const Sample *);

typedef struct WindowStat {
    int    n;                   /* valid (non-NA) samples                       */
    double min, max, median, p95, last;
    double max_age_s;           /* seconds before the reference time of the max */
    double min_age_s;
} WindowStat;

/* Indexes into WFT_METRICS. Keep in sync with the table in stats.c. */
enum MetricIndex {
    MI_WAKE_LATE, MI_PROBE_MS, MI_CPU, MI_DPC, MI_INTERRUPT,
    MI_CORE_CPU, MI_CORE_DPC, MI_CORE_INT, MI_PROC_QUEUE, MI_CTX_SWITCH,
    MI_DISK_QUEUE, MI_DISK_LAT, MI_DISK_WORST_LAT, MI_DISK_WORST_Q, MI_DISK_IOPS,
    MI_PAGES_INPUT, MI_PAGE_READS, MI_COMMIT_PCT, MI_MEM_LOAD, MI_AVAIL_MB,
    MI_NONPAGED_MB, MI_CPU_PERF, MI_HANDLES,
    MI_COUNT
};

typedef struct MetricDef {
    const char *id;             /* column-like identifier                       */
    const char *label;
    const char *unit;
    int         decimals;
    bool        lower_is_worse; /* e.g. available memory, CPU performance %     */
    MetricFn    fn;
} MetricDef;

extern const MetricDef WFT_METRICS[MI_COUNT];

/* Set every numeric Metrics field to WFT_NA and clear the name strings. */
void metrics_reset_na(Metrics *m);

/* Statistics over s[first .. last_excl). ref_mono_ms is the trigger time. */
void stats_window(const Sample *s, int first, int last_excl, MetricFn fn, double ref_mono_ms, WindowStat *out);

#endif
