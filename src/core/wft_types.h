/*
 * wft_types.h - shared data types for WinFreezeTrace.
 *
 * This header (and everything in src/core/) deliberately avoids <windows.h>
 * so the analysis logic can be unit-tested on any platform.
 */
#ifndef WFT_TYPES_H
#define WFT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define WFT_VERSION    "0.4.0"
#define WFT_MAX_RING   600          /* hard cap on retained samples        */
#define WFT_PATH_CCH   1024         /* wide-char capacity for paths        */

/* Metrics that could not be read are stored as WFT_NA (a negative value). */
#define WFT_NA         (-1.0)
#define WFT_HAS(v)     ((v) >= 0.0)

typedef struct WftTime {
    uint16_t year, month, day, hour, minute, second, millis;
} WftTime;

/* Result of the foreground-window responsiveness probe. */
typedef enum ProbeStatus {
    PROBE_NONE = 0,         /* no foreground window                          */
    PROBE_OK,               /* WM_NULL answered                              */
    PROBE_TIMEOUT,          /* WM_NULL timed out                             */
    PROBE_HUNG_FLAG,        /* SendMessageTimeout failed + IsHungAppWindow   */
    PROBE_DENIED,           /* blocked (UIPI / elevated target) - not a hang */
    PROBE_INVALID,          /* window vanished while probing                 */
    PROBE_SELF,             /* foreground window is WinFreezeTrace itself    */
    PROBE_SECURE_DESKTOP    /* UAC / lock screen: input desktop not Default  */
} ProbeStatus;

static inline bool probe_is_hung(int s) { return s == PROBE_TIMEOUT || s == PROBE_HUNG_FLAG; }

typedef enum TriggerReason {
    TRIGGER_NONE = 0,
    TRIGGER_SCHEDULER_STALL = 1,
    TRIGGER_APP_HANG = 2,
    TRIGGER_MANUAL = 3
} TriggerReason;

/*
 * One sampling interval. NOTE: every numeric field before the char arrays in
 * Metrics is a double so the collector can reset them to WFT_NA in one loop.
 */
typedef struct Metrics {
    /* CPU */
    double cpu, cpu_user, cpu_privileged, dpc, interrupt;
    double interrupts_per_sec, dpcs_queued_per_sec, context_switches_per_sec;
    double cpu_perf_pct, cpu_mhz;
    double core_cpu_max, core_dpc_max, core_interrupt_max;       /* worst single logical CPU */
    /* Disk */
    double disk_queue, disk_read_bps, disk_write_bps, disk_iops, disk_latency_ms;
    double disk_worst_latency_ms, disk_worst_queue;              /* worst single physical disk */
    /* Memory */
    double commit_pct, pages_input_per_sec, page_reads_per_sec, page_faults_per_sec;
    double pool_nonpaged_mb, pool_paged_mb, commit_used_mb, commit_limit_mb;
    double mem_load, avail_mb, commit_avail_mb;
    /* System */
    double proc_queue, system_handles, system_threads, system_processes;
    /* non-double members must stay below this line */
    char core_cpu_name[16], core_dpc_name[16], disk_worst_name[32];
} Metrics;

typedef struct Sample {
    WftTime ts;
    double  mono_ms;            /* monotonic time of the sample (QPC)           */
    int     wake_late_ms;       /* scheduler lateness of the sampling wake-up   */
    Metrics m;
    uint32_t fg_pid, fg_tid;
    int     probe_status;       /* ProbeStatus                                  */
    double  probe_ms;           /* time WM_NULL took to answer, or WFT_NA       */
    wchar_t fg_exe[64];         /* base name only: keeps the ring small         */
    wchar_t fg_class[64];
    wchar_t fg_title[160];
} Sample;

const char    *trigger_name(TriggerReason r);
const wchar_t *trigger_name_w(TriggerReason r);

#endif
