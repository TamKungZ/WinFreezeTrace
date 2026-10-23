#include "stats.h"

#include <stdlib.h>
#include <string.h>

#define ACC(name)  static double acc_##name(const Sample *s) { return s->m.name; }
ACC(cpu) ACC(dpc) ACC(interrupt) ACC(core_cpu_max) ACC(core_dpc_max) ACC(core_interrupt_max)
ACC(proc_queue) ACC(context_switches_per_sec) ACC(disk_queue) ACC(disk_latency_ms)
ACC(disk_worst_latency_ms) ACC(disk_worst_queue) ACC(disk_iops) ACC(pages_input_per_sec)
ACC(page_reads_per_sec) ACC(commit_pct) ACC(mem_load) ACC(avail_mb) ACC(pool_nonpaged_mb)
ACC(cpu_perf_pct) ACC(system_handles)
static double acc_wake_late(const Sample *s) { return (double)s->wake_late_ms; }
static double acc_probe_ms(const Sample *s)  { return s->probe_ms; }

const MetricDef WFT_METRICS[MI_COUNT] = {
    [MI_WAKE_LATE]      = { "wake_late_ms",   "Watchdog wake lateness",        "ms",   0, false, acc_wake_late },
    [MI_PROBE_MS]       = { "probe_ms",       "Foreground WM_NULL latency",    "ms",   1, false, acc_probe_ms },
    [MI_CPU]            = { "cpu_pct",        "CPU total",                     "%",    1, false, acc_cpu },
    [MI_DPC]            = { "dpc_pct",        "DPC time (all CPUs)",           "%",    1, false, acc_dpc },
    [MI_INTERRUPT]      = { "interrupt_pct",  "Interrupt time (all CPUs)",     "%",    1, false, acc_interrupt },
    [MI_CORE_CPU]       = { "core_cpu_max",   "Busiest logical CPU",           "%",    1, false, acc_core_cpu_max },
    [MI_CORE_DPC]       = { "core_dpc_max",   "Worst single-CPU DPC time",     "%",    1, false, acc_core_dpc_max },
    [MI_CORE_INT]       = { "core_int_max",   "Worst single-CPU interrupt",    "%",    1, false, acc_core_interrupt_max },
    [MI_PROC_QUEUE]     = { "proc_queue",     "Processor queue length",        "",     1, false, acc_proc_queue },
    [MI_CTX_SWITCH]     = { "ctx_switches",   "Context switches/s",            "/s",   0, false, acc_context_switches_per_sec },
    [MI_DISK_QUEUE]     = { "disk_queue",     "Disk queue (all disks)",        "",     2, false, acc_disk_queue },
    [MI_DISK_LAT]       = { "disk_lat_ms",    "Disk latency (avg of disks)",   "ms",   1, false, acc_disk_latency_ms },
    [MI_DISK_WORST_LAT] = { "disk_worst_lat", "Worst single-disk latency",     "ms",   1, false, acc_disk_worst_latency_ms },
    [MI_DISK_WORST_Q]   = { "disk_worst_q",   "Worst single-disk queue",       "",     2, false, acc_disk_worst_queue },
    [MI_DISK_IOPS]      = { "disk_iops",      "Disk transfers/s",              "/s",   0, false, acc_disk_iops },
    [MI_PAGES_INPUT]    = { "pages_input",    "Pages input/s",                 "/s",   0, false, acc_pages_input_per_sec },
    [MI_PAGE_READS]     = { "page_reads",     "Page reads/s",                  "/s",   0, false, acc_page_reads_per_sec },
    [MI_COMMIT_PCT]     = { "commit_pct",     "Commit in use",                 "%",    1, false, acc_commit_pct },
    [MI_MEM_LOAD]       = { "mem_load_pct",   "Physical memory load",          "%",    0, false, acc_mem_load },
    [MI_AVAIL_MB]       = { "avail_mb",       "Available memory",              "MB",   0, true,  acc_avail_mb },
    [MI_NONPAGED_MB]    = { "nonpaged_mb",    "Nonpaged pool",                 "MB",   0, false, acc_pool_nonpaged_mb },
    [MI_CPU_PERF]       = { "cpu_perf_pct",   "CPU performance (vs base)",     "%",    0, true,  acc_cpu_perf_pct },
    [MI_HANDLES]        = { "handles",        "System handle count",           "",     0, false, acc_system_handles },
};

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void stats_window(const Sample *s, int first, int last_excl, MetricFn fn, double ref_mono_ms, WindowStat *o) {
    double tmp[WFT_MAX_RING];
    int n = 0, imax = -1, imin = -1;
    o->n = 0; o->min = o->max = o->median = o->p95 = o->last = WFT_NA;
    o->max_age_s = o->min_age_s = WFT_NA;
    if (first < 0) first = 0;
    for (int i = first; i < last_excl && n < WFT_MAX_RING; ++i) {
        double v = fn(&s[i]);
        if (!WFT_HAS(v)) continue;
        if (imax < 0 || v > tmp[imax]) imax = n;
        if (imin < 0 || v < tmp[imin]) imin = n;
        tmp[n] = v;
        o->last = v;
        /* remember the sample index of extrema through parallel lookup below */
        ++n;
    }
    if (n == 0) return;
    o->n = n;
    o->max = tmp[imax];
    o->min = tmp[imin];
    /* Recover timestamps of the extrema (second pass; small n). */
    int k = 0;
    for (int i = first; i < last_excl && k < n; ++i) {
        double v = fn(&s[i]);
        if (!WFT_HAS(v)) continue;
        if (k == imax) o->max_age_s = (ref_mono_ms - s[i].mono_ms) / 1000.0;
        if (k == imin) o->min_age_s = (ref_mono_ms - s[i].mono_ms) / 1000.0;
        ++k;
    }
    qsort(tmp, (size_t)n, sizeof tmp[0], cmp_d);
    o->median = (n & 1) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
    int p = (int)((95 * n + 99) / 100) - 1;
    if (p < 0) p = 0;
    if (p >= n) p = n - 1;
    o->p95 = tmp[p];
}

void metrics_reset_na(Metrics *m) {
    memset(m, 0, sizeof *m);
    double *d = (double *)m;
    size_t n = offsetof(Metrics, core_cpu_name) / sizeof(double);
    for (size_t i = 0; i < n; ++i) d[i] = WFT_NA;
}
