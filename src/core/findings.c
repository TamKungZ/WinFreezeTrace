#include "findings.h"
#include "strutil.h"

#include <stdarg.h>
#include <string.h>

const char *confidence_name(Confidence c) {
    static const char *const n[] = { "INFO", "LOW", "MED", "HIGH" };
    return (c >= 0 && c <= CONF_HIGH) ? n[c] : "?";
}

static void add(AnalysisResult *r, Confidence c, const char *id, const char *title, const char *next, const char *evfmt, ...) {
    if (r->count >= WFT_MAX_FINDINGS) return;
    Finding *f = &r->f[r->count++];
    f->conf = c;
    wft_snprintf(f->id, sizeof f->id, "%s", id);
    wft_snprintf(f->title, sizeof f->title, "%s", title);
    wft_snprintf(f->next, sizeof f->next, "%s", next);
    va_list ap;
    va_start(ap, evfmt);
    vsnprintf(f->evidence, sizeof f->evidence, evfmt, ap);
    va_end(ap);
    f->evidence[sizeof f->evidence - 1] = 0;
}

/* "450.2 ms at t-3s (baseline median 2.1)" for the worst value in the lead window. */
static void peak(char *b, size_t n, const AnalysisResult *r, int mi) {
    const MetricDef *d = &WFT_METRICS[mi];
    const WindowStat *l = &r->lead[mi], *base = &r->base[mi];
    double v   = d->lower_is_worse ? l->min : l->max;
    double age = d->lower_is_worse ? l->min_age_s : l->max_age_s;
    if (!WFT_HAS(v)) { wft_snprintf(b, n, "n/a"); return; }
    int k = wft_snprintf(b, n, "%.*f%s%s at t-%.0fs", d->decimals, v, d->unit[0] ? " " : "", d->unit, age < 0 ? 0 : age);
    if (WFT_HAS(base->median) && (size_t)k < n)
        wft_snprintf(b + k, n - (size_t)k, " (baseline median %.*f)", d->decimals, base->median);
}


/* The sample inside the lead window where metric mi peaked (names such as the worst disk live there). */
static const Sample *peak_sample(const AnalysisInput *in, const AnalysisResult *r, int mi) {
    const MetricDef *d = &WFT_METRICS[mi];
    double want = d->lower_is_worse ? r->lead[mi].min : r->lead[mi].max;
    for (int i = in->n - 1; i >= r->lead_start && i >= 0; --i)
        if (d->fn(&in->s[i]) == want) return &in->s[i];
    return &in->s[in->n - 1];
}

static const char *or_q(const char *s) { return s && s[0] ? s : "?"; }

static void event_rule(AnalysisResult *r, const EventSummary *E, EventClass c, Confidence near_c, Confidence far_c,
                       const char *id, const char *title_near, const char *title_far, const char *next) {
    const EventCatStat *k = &E->cat[c];
    if (!k->total) return;
    bool near = k->near_count > 0;
    add(r, near ? near_c : far_c, id, near ? title_near : title_far, next,
        "%d event(s) in the last 30 min, %d within %.0f s of the trigger; nearest %.0f s away (%s). See events_digest.csv.",
        k->total, k->near_count, WFT_NEAR_EVENT_S, k->nearest_age_s, k->example);
}

static int highest_conf(const AnalysisResult *r) {
    int h = -1;
    for (int i = 0; i < r->count; ++i) if ((int)r->f[i].conf > h) h = (int)r->f[i].conf;
    return h;
}

void analysis_run(const AnalysisInput *in, AnalysisResult *r) {
    memset(r, 0, sizeof *r);
    const Sample *s = in->s;
    int n = in->n;
    if (n <= 0 || !s) {
        add(r, CONF_LOW, "no_data", "The timeline was empty, so no metric analysis was possible",
            "Check winfreezetrace.log; the recorder may have been paused or just started.", "No samples were available.");
        return;
    }
    double ref = n > 0 ? s[n - 1].mono_ms : 0.0;
    int ls = 0;
    while (ls < n && s[ls].mono_ms < ref - (double)in->lead_seconds * 1000.0) ++ls;
    r->lead_start = ls;
    int base_end = ls;
    if (base_end < 8) { base_end = n; r->baseline_overlaps = true; }
    for (int i = 0; i < MI_COUNT; ++i) {
        stats_window(s, ls, n, WFT_METRICS[i].fn, ref, &r->lead[i]);
        stats_window(s, 0, base_end, WFT_METRICS[i].fn, ref, &r->base[i]);
        stats_window(s, 0, n, WFT_METRICS[i].fn, ref, &r->all[i]);
    }

#define LMAX(i) (r->lead[i].max)
#define LMIN(i) (r->lead[i].min)
    char p1[160], p2[160], p3[160];
    const EventSummary *E = &in->ev;
    uint32_t cpus = in->logical_cpus ? in->logical_cpus : 1;

    /* direct evidence of where the wait was */
    if (in->wait_cycle)
        add(r, CONF_HIGH, "wait_cycle", "Deadlock: circular wait chain in the foreground process",
            "Open waitchains.txt, find the thread marked cycle=YES, and load target.dmp in WinDbg (!analyze -v, ~*k) with symbols.",
            "Wait Chain Traversal reported cycle=YES for at least one thread.");
    if (in->ui_blocker[0])
        add(r, in->ui_cross_process ? CONF_MED : CONF_LOW, "ui_blocked",
            in->ui_cross_process ? "Foreground UI thread was waiting on another process" : "Foreground UI thread was waiting on a lock/thread inside the app",
            in->ui_cross_process ? "Identify the blocking process in waitchains.txt; check whether it was itself busy (processes.csv) or hung (Event Log)."
                                 : "Inspect the owner thread in waitchains.txt/threads.csv; a debugger on target.dmp shows its stack.",
            "UI thread chain: %s", in->ui_blocker);

    /* Event Log correlation (near = within 3 minutes of the trigger) */
    event_rule(r, E, EVC_WHEA_UNCORRECTED, CONF_HIGH, CONF_MED, "whea_fatal",
        "Uncorrected hardware error (WHEA) near the incident", "Uncorrected hardware error (WHEA) earlier in the last 30 minutes",
        "Check CPU/RAM/PCIe/GPU/storage health, firmware and cooling. Run vendor diagnostics; consider reverting overclocks.");
    event_rule(r, E, EVC_WHEA_CORRECTED, CONF_MED, CONF_LOW, "whea_corrected",
        "Corrected hardware errors (WHEA) near the incident", "Corrected hardware errors (WHEA) earlier in the last 30 minutes",
        "Corrected errors are often benign, but bursts point to marginal hardware (PCIe link, RAM, undervolt/overclock).");
    event_rule(r, E, EVC_LIVEKERNEL, CONF_HIGH, CONF_MED, "livekernel",
        "Windows recorded a LiveKernelEvent (driver/GPU watchdog)", "A LiveKernelEvent was recorded earlier in the last 30 minutes",
        "Look at the LiveKernelEvent code in events_digest.csv/events_application.xml (e.g. 141/117 = GPU hang). Update or roll back the GPU driver; check power and temperature.");
    event_rule(r, E, EVC_DISPLAY, CONF_HIGH, CONF_LOW, "display_driver",
        "GPU/display driver event near the incident (possible TDR/reset)", "GPU/display driver events earlier in the last 30 minutes",
        "Compare the driver version in display_drivers.csv with vendor releases; try a clean driver install and check GPU temperature/power limits.");
    event_rule(r, E, EVC_STORAGE, CONF_HIGH, CONF_LOW, "storage_events",
        "Storage driver/disk errors near the incident", "Storage driver/disk events earlier in the last 30 minutes",
        "Check SMART/NVMe health, cables and firmware (storage_devices.csv), and whether the same disk shows the worst latency in the timeline.");
    event_rule(r, E, EVC_FILESYSTEM, CONF_MED, CONF_LOW, "filesystem_events",
        "File-system / volume event near the incident", "File-system / volume events earlier in the last 30 minutes",
        "Run chkdsk (read-only first) on the volume named in the event and check disk health.");
    event_rule(r, E, EVC_THROTTLE, CONF_MED, CONF_LOW, "cpu_throttle_event",
        "Firmware limited CPU speed near the incident", "Firmware limited CPU speed earlier in the last 30 minutes",
        "Check cooling, power plan, VRM/PSU limits and BIOS power settings; see power.txt.");
    event_rule(r, E, EVC_APPHANG, CONF_MED, CONF_LOW, "app_hang_event",
        "Windows logged an Application Hang near the incident", "Application Hang events earlier in the last 30 minutes",
        "Match the application name/time in events_digest.csv with foreground.txt.");
    event_rule(r, E, EVC_APPCRASH, CONF_LOW, CONF_INFO, "app_crash_event",
        "An application crash was logged near the incident", "Application crashes were logged earlier in the last 30 minutes",
        "A crash can follow (or cause) a hang; check the faulting module in events_digest.csv.");
    event_rule(r, E, EVC_BUGCHECK, CONF_MED, CONF_MED, "bugcheck_event",
        "A previous blue screen (bugcheck) was recorded recently", "A previous blue screen (bugcheck) was recorded recently",
        "Analyse the memory dump in C:\\Windows\\Minidump with WinDbg or WhoCrashed; recurring bugchecks are the strongest lead.");
    event_rule(r, E, EVC_UNEXPECTED_SHUTDOWN, CONF_LOW, CONF_LOW, "unexpected_shutdown",
        "A previous unexpected shutdown/power loss was recorded recently", "A previous unexpected shutdown/power loss was recorded recently",
        "If earlier freezes ended with a forced power-off, suspect power delivery or a hard lock rather than software.");
    if (E->cat[EVC_SLEEP_RESUME].near_count > 0 || in->resumed_recently)
        add(r, CONF_INFO, "sleep_resume", "The system slept or resumed around this incident",
            in->reason == TRIGGER_SCHEDULER_STALL ? "A scheduler stall right after resume is usually an artefact of the transition, not a freeze." : "Resume transitions can make apps briefly unresponsive.",
            "Sleep/resume notifications were seen within the last minutes (events: %d near).", E->cat[EVC_SLEEP_RESUME].near_count);

    /* metric correlation over the lead-in window */
    double commit = LMAX(MI_COMMIT_PCT), load = LMAX(MI_MEM_LOAD), avail = LMIN(MI_AVAIL_MB);
    if ((WFT_HAS(commit) && commit >= 95.0) || (WFT_HAS(load) && load >= 97.0) || (WFT_HAS(avail) && avail < 300.0)) {
        peak(p1, sizeof p1, r, MI_COMMIT_PCT); peak(p2, sizeof p2, r, MI_MEM_LOAD); peak(p3, sizeof p3, r, MI_AVAIL_MB);
        add(r, CONF_MED, "memory_pressure", "Memory/commit pressure before the incident",
            "Open processes.csv sorted by private_MB, check the page file size and for leaks; add RAM or close the largest consumers.",
            "commit %s; load %s; available %s.", p1, p2, p3);
    }
    double dpc = LMAX(MI_DPC), itr = LMAX(MI_INTERRUPT), cdpc = LMAX(MI_CORE_DPC), cint = LMAX(MI_CORE_INT);
    if ((WFT_HAS(dpc) && dpc >= 15.0) || (WFT_HAS(itr) && itr >= 15.0) || (WFT_HAS(cdpc) && cdpc >= 40.0) || (WFT_HAS(cint) && cint >= 40.0)) {
        peak(p1, sizeof p1, r, MI_DPC); peak(p2, sizeof p2, r, MI_CORE_DPC); peak(p3, sizeof p3, r, MI_INTERRUPT);
        add(r, CONF_MED, "dpc_interrupt", "High DPC/interrupt time (driver or device latency)",
            "Record a short ETW trace with WPR (CPU usage + DPC/ISR) or run LatencyMon, then check the loaded third-party drivers in loaded_drivers.csv (network, audio, storage, GPU).",
            "DPC %s; worst single-CPU DPC %s (CPU %s); interrupt %s.", p1, p2, or_q(peak_sample(in, r, MI_CORE_DPC)->m.core_dpc_name), p3);
    }
    double cpu = LMAX(MI_CPU), pq = LMAX(MI_PROC_QUEUE);
    if (WFT_HAS(cpu) && cpu >= 97.0 && WFT_HAS(pq) && pq > (double)cpus) {
        peak(p1, sizeof p1, r, MI_CPU); peak(p2, sizeof p2, r, MI_PROC_QUEUE);
        add(r, CONF_MED, "cpu_saturation", "CPU saturation with a long run queue",
            "See the top CPU processes and hottest threads in this report; check for a runaway process or background scan.",
            "CPU %s; processor queue %s with %u logical CPUs.", p1, p2, (unsigned)cpus);
    }
    double dq = LMAX(MI_DISK_QUEUE), wq = LMAX(MI_DISK_WORST_Q);
    if (((WFT_HAS(dq) && dq >= 4.0) || (WFT_HAS(wq) && wq >= 4.0)) && !(WFT_HAS(cpu) && cpu >= 90.0)) {
        peak(p1, sizeof p1, r, MI_DISK_QUEUE); peak(p2, sizeof p2, r, MI_DISK_WORST_Q);
        add(r, CONF_MED, "disk_queue", "Disk queue was elevated without CPU saturation",
            "Compare with the Top I/O processes below and with storage events; a deep queue on one disk points at that device.",
            "queue (avg) %s; worst disk %s [%s].", p1, p2, or_q(peak_sample(in, r, MI_DISK_WORST_Q)->m.disk_worst_name));
    }
    double lat = LMAX(MI_DISK_LAT), wlat = LMAX(MI_DISK_WORST_LAT);
    double worst_lat = (WFT_HAS(lat) ? lat : -1.0);
    if (WFT_HAS(wlat) && wlat > worst_lat) worst_lat = wlat;
    if (worst_lat >= 100.0) {
        peak(p1, sizeof p1, r, MI_DISK_LAT); peak(p2, sizeof p2, r, MI_DISK_WORST_LAT);
        add(r, worst_lat >= 1000.0 ? CONF_HIGH : CONF_MED, "disk_latency", "Storage response time was very high",
            "Identify the slow device (disk name in this finding), check SMART/NVMe health and firmware, and see Top I/O processes.",
            "avg latency %s; worst disk %s [%s].", p1, p2, or_q(peak_sample(in, r, MI_DISK_WORST_LAT)->m.disk_worst_name));
    }
    double pin = LMAX(MI_PAGES_INPUT), prd = LMAX(MI_PAGE_READS);
    if ((WFT_HAS(pin) && pin >= 1000.0) || (WFT_HAS(prd) && prd >= 250.0)) {
        peak(p1, sizeof p1, r, MI_PAGES_INPUT); peak(p2, sizeof p2, r, MI_PAGE_READS);
        add(r, CONF_MED, "hard_paging", "Heavy hard paging",
            "Memory pressure or a slow page file; check available memory and page-file location/speed.",
            "pages input %s; page reads %s.", p1, p2);
    }
    double perf = LMIN(MI_CPU_PERF);
    if (WFT_HAS(perf) && perf <= 60.0 && WFT_HAS(cpu) && cpu >= 80.0) {
        peak(p1, sizeof p1, r, MI_CPU_PERF);
        add(r, CONF_MED, "cpu_downclock", "CPU ran well below its base performance while busy",
            "Check the power plan, cooling and BIOS power limits (power.txt); battery saver and thermal limits are common causes.",
            "CPU performance %s while CPU load reached %.0f%%.", p1, cpu);
    }
    if (WFT_HAS(in->max_temp_c) && in->max_temp_c >= 95.0)
        add(r, CONF_MED, "thermal_high", "A thermal zone reported a very high temperature", "Check cooling, dust and fan curves.",
            "Hottest thermal zone: %.0f C.", in->max_temp_c);
    if (WFT_HAS(in->min_passive_limit_pct) && in->min_passive_limit_pct < 90.0)
        add(r, CONF_MED, "thermal_throttle", "Passive thermal limiting was active", "Check cooling; see power.txt.",
            "Passive limit at %.0f%% of maximum.", in->min_passive_limit_pct);
    if (WFT_HAS(in->lowest_free_pct) && (in->lowest_free_pct < 5.0 || (WFT_HAS(in->lowest_free_gb) && in->lowest_free_gb < 5.0)))
        add(r, CONF_MED, "low_disk_space", "A fixed volume is nearly full",
            "Free space on the system/page-file volume; a nearly full SSD slows writes and can stall paging.",
            "Drive %s has %.1f%% (%.1f GB) free.", in->lowest_free_drive, in->lowest_free_pct, WFT_HAS(in->lowest_free_gb) ? in->lowest_free_gb : 0.0);
    double npp = LMAX(MI_NONPAGED_MB);
    if (WFT_HAS(npp) && npp > 2048.0) {
        peak(p1, sizeof p1, r, MI_NONPAGED_MB);
        add(r, CONF_LOW, "nonpaged_pool", "Nonpaged pool is unusually large", "A driver may be leaking; compare against loaded_drivers.csv and use poolmon.", "nonpaged pool %s.", p1);
    }

    /* trigger-specific interpretation */
    if (in->reason == TRIGGER_APP_HANG && WFT_HAS(LMAX(MI_WAKE_LATE)) && LMAX(MI_WAKE_LATE) < (double)in->stall_ms)
        add(r, CONF_MED, "app_specific_hang", "Only the foreground app stopped responding; the watchdog kept scheduling normally",
            "This favours an application-specific wait/deadlock over a system-wide freeze. Start with waitchains.txt and target.dmp.",
            "Max watchdog lateness in the lead window was %.0f ms (threshold %u ms).", LMAX(MI_WAKE_LATE), (unsigned)in->stall_ms);
    double pm = LMAX(MI_PROBE_MS);
    if (in->reason != TRIGGER_APP_HANG && WFT_HAS(pm) && pm >= 100.0) {
        peak(p1, sizeof p1, r, MI_PROBE_MS);
        add(r, CONF_LOW, "ui_latency", "The foreground UI thread was slow to answer messages",
            "A busy or blocked UI thread causes visible stutter even when the system is idle; see hot threads and wait chains.", "WM_NULL latency %s.", p1);
    }
    if (in->reason == TRIGGER_STORAGE_STALL) {
        bool have = false;
        for (int i = 0; i < r->count; ++i) if (!strcmp(r->f[i].id, "disk_latency") || !strcmp(r->f[i].id, "disk_queue")) have = true;
        if (!have)
            add(r, CONF_MED, "storage_trigger", "A disk was slow or backed up when the capture started",
                "Look at the disk columns in timeline.csv and the storage entries in events_digest.csv; check SMART/NVMe health, cables and the storage driver.",
                "Worst disk at trigger: latency %.1f ms, queue %.1f [%s].", WFT_HAS(s[n - 1].m.disk_worst_latency_ms) ? s[n - 1].m.disk_worst_latency_ms : 0.0,
                WFT_HAS(s[n - 1].m.disk_worst_queue) ? s[n - 1].m.disk_worst_queue : 0.0, or_q(s[n - 1].m.disk_worst_name));
    }
    if (in->reason == TRIGGER_MANUAL)
        add(r, CONF_INFO, "manual", "Manual capture", "Findings below describe the lead-in to the key press; no automatic trigger fired.", "Triggered with the hotkey.");
    if (in->reason == TRIGGER_SCHEDULER_STALL && highest_conf(r) < (int)CONF_MED)
        add(r, CONF_LOW, "stall_unexplained", "The watchdog was delayed but no strong CPU/memory/disk/event signature was captured",
            "Suspect a low-level driver, firmware, GPU or storage stall. target.dmp only covers the foreground app; if this repeats, record a bounded kernel ETW trace (WPR) and enable kernel dumps.",
            "Wake lateness at trigger: %d ms (threshold %u ms).", s[n - 1].wake_late_ms, (unsigned)in->stall_ms);
    if (r->count == 0)
        add(r, CONF_LOW, "no_signature", "No single cause is strong enough from this capture",
            "Use timeline.csv, processes.csv, waitchains.txt and the Event Log digest together.", "No rule matched.");

    /* stable sort: highest confidence first */
    for (int i = 1; i < r->count; ++i) {
        Finding t = r->f[i]; int j = i - 1;
        while (j >= 0 && r->f[j].conf < t.conf) { r->f[j + 1] = r->f[j]; --j; }
        r->f[j + 1] = t;
    }
}

void analysis_write_findings_text(FILE *f, const AnalysisResult *r) {
    fputs("Findings (ranked by confidence)\r\n-------------------------------\r\n", f);
    for (int i = 0; i < r->count; ++i) {
        const Finding *x = &r->f[i];
        fprintf(f, "%2d. [%s] %s\r\n      Evidence: %s\r\n      Next:     %s\r\n", i + 1, confidence_name(x->conf), x->title, x->evidence, x->next);
    }
}

static void numcell(char *b, size_t n, double v, int dec) {
    if (!WFT_HAS(v)) wft_snprintf(b, n, "n/a"); else wft_snprintf(b, n, "%.*f", dec, v);
}

void analysis_write_metric_table(FILE *f, const AnalysisResult *r, uint32_t lead_seconds) {
    fprintf(f, "Metrics: baseline vs last %u s before the trigger%s\r\n", (unsigned)lead_seconds,
            r->baseline_overlaps ? " (history too short: baseline includes the lead window)" : "");
    fputs("--------------------------------------------------------------------------------------\r\n", f);
    fprintf(f, "%-32s %10s %10s %16s %10s\r\n", "Metric", "base med", "base p95", "lead worst", "at t-s");
    for (int i = 0; i < MI_COUNT; ++i) {
        const MetricDef *d = &WFT_METRICS[i];
        char a[24], b[24], c[24], e[24];
        numcell(a, sizeof a, r->base[i].median, d->decimals);
        numcell(b, sizeof b, r->base[i].p95, d->decimals);
        numcell(c, sizeof c, d->lower_is_worse ? r->lead[i].min : r->lead[i].max, d->decimals);
        numcell(e, sizeof e, d->lower_is_worse ? r->lead[i].min_age_s : r->lead[i].max_age_s, 0);
        char label[48]; wft_snprintf(label, sizeof label, "%s%s%s%s", d->label, d->unit[0] ? " (" : "", d->unit, d->unit[0] ? ")" : "");
        fprintf(f, "%-32s %10s %10s %16s %10s%s\r\n", label, a, b, c, e, d->lower_is_worse ? "  (lower is worse)" : "");
    }
}

void analysis_write_event_summary(FILE *f, const EventSummary *e) {
    fputs("Event Log correlation (last 30 min; near = within 3 min of the trigger)\r\n"
          "---------------------------------------------------------------------\r\n", f);
    bool any = false;
    for (int i = 1; i < EVC_COUNT; ++i) {
        const EventCatStat *k = &e->cat[i];
        if (!k->total) continue;
        any = true;
        fprintf(f, "%-24s total %3d  near %3d  nearest %4.0f s  (%s)\r\n", event_class_name((EventClass)i), k->total, k->near_count,
                k->nearest_age_s, k->example);
    }
    if (!any) fputs("No classified warning/error/critical events were found in the window.\r\n", f);
}

void analysis_write_findings_json(FILE *f, const AnalysisResult *r) {
    fputs("[", f);
    for (int i = 0; i < r->count; ++i) {
        const Finding *x = &r->f[i];
        fputs(i ? ",\n    {" : "\n    {", f);
        fprintf(f, "\"confidence\": \"%s\", \"id\": ", confidence_name(x->conf)); wft_json_str(f, x->id);
        fputs(", \"title\": ", f);    wft_json_str(f, x->title);
        fputs(", \"evidence\": ", f); wft_json_str(f, x->evidence);
        fputs(", \"next\": ", f);     wft_json_str(f, x->next);
        fputs("}", f);
    }
    fputs(r->count ? "\n  ]" : "]", f);
}

static void jnum(FILE *f, double v) { if (WFT_HAS(v)) fprintf(f, "%.4f", v); else fputs("null", f); }

void analysis_write_metrics_json(FILE *f, const AnalysisResult *r) {
    fputs("{", f);
    for (int i = 0; i < MI_COUNT; ++i) {
        const MetricDef *d = &WFT_METRICS[i];
        fprintf(f, "%s\n    \"%s\": {\"unit\": \"%s\", \"baseline_median\": ", i ? "," : "", d->id, d->unit);
        jnum(f, r->base[i].median);
        fputs(", \"baseline_p95\": ", f); jnum(f, r->base[i].p95);
        fputs(", \"lead_worst\": ", f);   jnum(f, d->lower_is_worse ? r->lead[i].min : r->lead[i].max);
        fputs(", \"lead_worst_age_s\": ", f); jnum(f, d->lower_is_worse ? r->lead[i].min_age_s : r->lead[i].max_age_s);
        fputs("}", f);
    }
    fputs("\n  }", f);
}
