#include "report.h"

#include "dump.h"
#include "events.h"
#include "findings.h"
#include "procsnap.h"
#include "retention.h"
#include "sysinfo.h"
#include "threadinfo.h"

/* capture bookkeeping */

typedef struct Cap {
    const Config *cfg;
    WCHAR dir[WFT_PATH_CCH];
    struct { const char *name; double ms; } steps[24];
    int ns;
    char warn[16][200];
    int nw;
    const char *cur; double t_step;
} Cap;

static void step_begin(Cap *c, const char *name) { c->cur = name; c->t_step = plat_qpc_ms(); }
static void step_end(Cap *c) {
    if (c->ns < 24) { c->steps[c->ns].name = c->cur; c->steps[c->ns].ms = plat_qpc_ms() - c->t_step; ++c->ns; }
}
static void warn(Cap *c, const char *fmt, ...) {
    if (c->nw >= 16) return;
    va_list ap; va_start(ap, fmt); vsnprintf(c->warn[c->nw], sizeof c->warn[0], fmt, ap); va_end(ap);
    c->warn[c->nw][sizeof c->warn[0] - 1] = 0;
    log_msg("WARN", "capture: %s", c->warn[c->nw]);
    ++c->nw;
}

/* retention (file-system half) */

static bool scan_incident_dir(const WCHAR *dir, bool do_delete) {
    WCHAR pat[WFT_PATH_CCH]; wft_wsnprintf(pat, WFT_PATH_CCH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        /* Refuse anything we did not create: sub-folders, links, unknown file types. */
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !retention_file_name_ok(fd.cFileName)) { ok = false; break; }
        if (do_delete) {
            WCHAR fp[WFT_PATH_CCH]; wft_wsnprintf(fp, WFT_PATH_CCH, L"%ls\\%ls", dir, fd.cFileName);
            if (!DeleteFileW(fp)) ok = false;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

void report_prune(const WCHAR *root, uint32_t keep) {
    if (!keep) return;
    enum { MAXN = 4096, NAMECCH = 48 };
    WCHAR (*names)[NAMECCH] = (WCHAR (*)[NAMECCH])calloc(MAXN, sizeof *names);
    const WCHAR **ptrs = (const WCHAR **)calloc(MAXN, sizeof *ptrs);
    int *del = (int *)calloc(MAXN, sizeof *del);
    if (!names || !ptrs || !del) goto out;
    WCHAR pat[WFT_PATH_CCH]; wft_wsnprintf(pat, WFT_PATH_CCH, L"%ls\\*", root);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) goto out;
    int n = 0;
    do {
        if (n >= MAXN) break;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (wcslen(fd.cFileName) >= NAMECCH || !retention_dir_name_ok(fd.cFileName)) continue;
        wcscpy(names[n], fd.cFileName); ptrs[n] = names[n]; ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    int k = retention_select(ptrs, n, (int)keep, del);
    for (int i = 0; i < k; ++i) {
        WCHAR dp[WFT_PATH_CCH]; wft_wsnprintf(dp, WFT_PATH_CCH, L"%ls\\%ls", root, names[del[i]]);
        if (scan_incident_dir(dp, false) && scan_incident_dir(dp, true) && RemoveDirectoryW(dp)) log_msg_w("INFO", "retention: removed old report %s", names[del[i]]);
        else log_msg_w("WARN", "retention: left %s in place (unexpected contents or in use)", names[del[i]]);
    }
out:
    free(names); free(ptrs); free(del);
}

/* incident folder + timeline */

static bool make_incident_dir(const Config *cfg, const WftTime *t, TriggerReason why, WCHAR *out, size_t cch) {
    plat_create_private_dir(cfg->report_root);
    for (int i = 0; i < 100; ++i) {
        if (i == 0) wft_wsnprintf(out, cch, L"%ls\\%04u%02u%02u_%02u%02u%02u_%ls", cfg->report_root, t->year, t->month, t->day, t->hour, t->minute, t->second, trigger_name_w(why));
        else        wft_wsnprintf(out, cch, L"%ls\\%04u%02u%02u_%02u%02u%02u_%ls_%d", cfg->report_root, t->year, t->month, t->day, t->hour, t->minute, t->second, trigger_name_w(why), i + 1);
        DirResult r = plat_create_private_dir(out);
        if (r == DIR_CREATED) return true;
        if (r == DIR_FAILED) return false;       /* exists = collision: try the next suffix */
    }
    return false;
}

typedef struct Col { const char *name; size_t off; int dec; } Col;
#define COL(n, f, d) { n, offsetof(Metrics, f), d }
static const Col k_cols[] = {
    COL("cpu_pct", cpu, 2), COL("cpu_user_pct", cpu_user, 2), COL("cpu_privileged_pct", cpu_privileged, 2), COL("dpc_pct", dpc, 2),
    COL("interrupt_pct", interrupt, 2), COL("interrupts_per_sec", interrupts_per_sec, 0), COL("dpcs_queued_per_sec", dpcs_queued_per_sec, 0),
    COL("context_switches_per_sec", context_switches_per_sec, 0), COL("cpu_perf_pct", cpu_perf_pct, 0), COL("cpu_mhz", cpu_mhz, 0),
    COL("core_cpu_max_pct", core_cpu_max, 1), COL("core_dpc_max_pct", core_dpc_max, 1), COL("core_interrupt_max_pct", core_interrupt_max, 1),
    COL("disk_queue", disk_queue, 3), COL("disk_read_Bps", disk_read_bps, 0), COL("disk_write_Bps", disk_write_bps, 0), COL("disk_IOPS", disk_iops, 1),
    COL("disk_latency_ms", disk_latency_ms, 3), COL("disk_worst_latency_ms", disk_worst_latency_ms, 3), COL("disk_worst_queue", disk_worst_queue, 3),
    COL("commit_pct", commit_pct, 2), COL("mem_load_pct", mem_load, 0), COL("avail_mem_MB", avail_mb, 0), COL("commit_avail_MB", commit_avail_mb, 0),
    COL("commit_used_MB", commit_used_mb, 0), COL("commit_limit_MB", commit_limit_mb, 0),
    COL("pages_input_per_sec", pages_input_per_sec, 1), COL("page_reads_per_sec", page_reads_per_sec, 1), COL("page_faults_per_sec", page_faults_per_sec, 1),
    COL("pool_nonpaged_MB", pool_nonpaged_mb, 1), COL("pool_paged_MB", pool_paged_mb, 1),
    COL("processor_queue", proc_queue, 2), COL("system_handles", system_handles, 0), COL("system_threads", system_threads, 0), COL("system_processes", system_processes, 0),
};

static bool write_timeline(Cap *c, const Sample *s, int n) {
    WCHAR p[WFT_PATH_CCH]; wft_wsnprintf(p, WFT_PATH_CCH, L"%ls\\timeline.csv", c->dir);
    FILE *f = plat_fopen_w(p); if (!f) return false;
    fputs("time,t_minus_s,wake_late_ms,probe_status,probe_ms", f);
    for (size_t i = 0; i < sizeof k_cols / sizeof k_cols[0]; ++i) fprintf(f, ",%s", k_cols[i].name);
    fputs(",worst_cpu_name,worst_dpc_cpu_name,worst_disk_name,fg_pid,fg_tid,fg_exe,fg_class,fg_title\r\n", f);
    double ref = s[n - 1].mono_ms;
    for (int i = 0; i < n; ++i) {
        const Sample *x = &s[i];
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%.1f,%d,%d,", x->ts.year, x->ts.month, x->ts.day, x->ts.hour, x->ts.minute, x->ts.second, x->ts.millis,
                (ref - x->mono_ms) / 1000.0, x->wake_late_ms, x->probe_status);
        if (WFT_HAS(x->probe_ms)) fprintf(f, "%.1f", x->probe_ms);
        for (size_t k = 0; k < sizeof k_cols / sizeof k_cols[0]; ++k) {
            double v = *(const double *)((const char *)&x->m + k_cols[k].off);
            fputc(',', f);
            if (WFT_HAS(v)) fprintf(f, "%.*f", k_cols[k].dec, v);         /* unavailable counters are left empty, not -1 */
        }
        fputc(',', f); wft_csv_utf8(f, x->m.core_cpu_name); fputc(',', f); wft_csv_utf8(f, x->m.core_dpc_name); fputc(',', f); wft_csv_utf8(f, x->m.disk_worst_name);
        fprintf(f, ",%lu,%lu,", (unsigned long)x->fg_pid, (unsigned long)x->fg_tid);
        wft_csv_text(f, x->fg_exe); fputc(',', f); wft_csv_text(f, x->fg_class); fputc(',', f);
        wft_csv_text(f, c->cfg->redact ? L"<redacted>" : x->fg_title); fputs("\r\n", f);
    }
    bool ok = ferror(f) == 0;
    return fclose(f) == 0 && ok;
}

/* summary */

typedef struct Outcome {
    const Sample *trig; const ForegroundInfo *fg; TriggerReason reason;
    AnalysisResult *an; EventsResult *ev;
    WaitInfo wi; ThreadSnap *ts; HotThread hot[5]; int nhot;
    TopProc top_cpu[5]; int ntop_cpu; TopProc top_io[5]; int ntop_io;
    DumpResult dump; DWORD dump_err; double dump_ms;
    double total_ms; int proc_rows; double proc_window_ms;
    char incident[96];
    int nsamples;
} Outcome;

static void write_summary_txt(Cap *c, const Outcome *o) {
    WCHAR p[WFT_PATH_CCH]; wft_wsnprintf(p, WFT_PATH_CCH, L"%ls\\summary.txt", c->dir);
    FILE *f = plat_fopen_w(p); if (!f) { warn(c, "could not write summary.txt"); return; }
    const Config *cfg = c->cfg; const Sample *t = o->trig;
    fputs("WinFreezeTrace incident report\r\n==============================\r\n", f);
    fprintf(f, "Version: %s\r\nTrigger: %s\r\nTime: %04u-%02u-%02u %02u:%02u:%02u.%03u local\r\nIncident folder: %s\r\n", WFT_VERSION, trigger_name(o->reason),
            t->ts.year, t->ts.month, t->ts.day, t->ts.hour, t->ts.minute, t->ts.second, t->ts.millis, o->incident);
    fprintf(f, "Wake lateness at trigger: %d ms (threshold %lu ms)\r\n", t->wake_late_ms, (unsigned long)cfg->stall_ms);
    if (WFT_HAS(t->m.disk_worst_latency_ms) || WFT_HAS(t->m.disk_worst_queue)) {
        fputs("Worst disk at trigger:", f);
        if (WFT_HAS(t->m.disk_worst_latency_ms)) fprintf(f, " latency %.1f ms (threshold %lu)", t->m.disk_worst_latency_ms, (unsigned long)cfg->disk_latency_ms);
        if (WFT_HAS(t->m.disk_worst_queue)) fprintf(f, ", queue %.1f (threshold %lu)", t->m.disk_worst_queue, (unsigned long)cfg->disk_queue);
        if (t->m.disk_worst_name[0]) fprintf(f, ", disk %s", t->m.disk_worst_name);
        fputs("\r\n", f);
    }
    WCHAR exe[WFT_PATH_CCH]; wft_wsnprintf(exe, WFT_PATH_CCH, L"%ls", o->fg->path);
    if (cfg->redact) wft_redact_user_path(exe, WFT_PATH_CCH);
    fprintf(f, "Foreground: PID %lu / TID %lu\r\n  EXE:   ", (unsigned long)o->fg->pid, (unsigned long)o->fg->tid); wft_write_utf8(f, exe);
    fputs("\r\n  Class: ", f); wft_write_utf8(f, o->fg->cls);
    fputs("\r\n  Title: ", f); wft_write_utf8(f, cfg->redact ? L"<redacted>" : o->fg->title);
    fprintf(f, "\r\n  Responsiveness probe: %s", probe_status_name(o->fg->status));
    if (WFT_HAS(o->fg->probe_ms)) fprintf(f, " (%.1f ms)", o->fg->probe_ms);
    fprintf(f, "\r\nSamples in timeline: %d (interval %lu ms, lead-in window %lu s)\r\n", o->nsamples, (unsigned long)cfg->interval_ms, (unsigned long)cfg->lead_seconds);
    for (int i = 0; i < cfg->warning_count; ++i) fprintf(f, "Config note: %s\r\n", cfg->warnings[i]);
    fputs("\r\n", f);

    analysis_write_findings_text(f, o->an);
    fputs("\r\n", f);
    analysis_write_metric_table(f, o->an, cfg->lead_seconds);

    fprintf(f, "\r\nTop processes during a %.0f ms window after the trigger (processes.csv has all %d)\r\n"
               "-----------------------------------------------------------------------------\r\n", o->proc_window_ms, o->proc_rows);
    for (int i = 0; i < o->ntop_cpu; ++i) fprintf(f, "  CPU  %5.1f%% of system  %s (pid %lu)\r\n", o->top_cpu[i].value, o->top_cpu[i].name, (unsigned long)o->top_cpu[i].pid);
    for (int i = 0; i < o->ntop_io; ++i)  fprintf(f, "  I/O  %7.1f MB/s        %s (pid %lu)\r\n", o->top_io[i].value / 1048576.0, o->top_io[i].name, (unsigned long)o->top_io[i].pid);
    if (!o->ntop_cpu && !o->ntop_io) fputs("  (no process showed measurable CPU or I/O in the window; the incident may already have ended)\r\n", f);
    fputs("  Note: this is measured AFTER the trigger fired. For what happened before it, use timeline.csv.\r\n", f);

    if (o->nhot) {
        fputs("\r\nHottest threads in the foreground process (threads.csv)\r\n-------------------------------------------------------\r\n", f);
        for (int i = 0; i < o->nhot; ++i) fprintf(f, "  thread %lu%s%s%s  %.0f%% of one core\r\n", (unsigned long)o->hot[i].tid, o->hot[i].gui ? " [GUI/UI thread]" : "",
                                                    o->hot[i].name[0] ? " name=" : "", o->hot[i].name, o->hot[i].pct);
    }
    fputs("\r\n", f);
    analysis_write_event_summary(f, &o->ev->summary);

    fputs("\r\nCapture details\r\n---------------\r\n", f);
    fprintf(f, "Minidump: %s", dump_result_text(o->dump));
    if (o->dump == DUMP_OK || o->dump == DUMP_TIMEOUT) fprintf(f, " (took %.0f ms; the target process is suspended while it is written)", o->dump_ms);
    if (o->dump_err && o->dump != DUMP_OK) fprintf(f, " [error 0x%08lx]", (unsigned long)o->dump_err);
    fprintf(f, "\r\nWait chains: %s; threads examined %d, waiting on something %d%s\r\n", o->wi.ok ? "ok" : "unavailable", o->wi.scanned, o->wi.in_chain,
            o->wi.budget_exhausted ? " (time budget reached)" : "");
    fprintf(f, "Event Log: %d of 5 queries ok, %d events in events_digest.csv\r\n", o->ev->channels_ok, o->ev->digest_rows);
    fprintf(f, "Capture cost: %.0f ms total (WinFreezeTrace itself was busy this long):", o->total_ms);
    for (int i = 0; i < c->ns; ++i) fprintf(f, " %s=%.0f", c->steps[i].name, c->steps[i].ms);
    fputs(" ms\r\n", f);
    for (int i = 0; i < c->nw; ++i) fprintf(f, "WARNING: %s\r\n", c->warn[i]);
    if (cfg->redact) fputs("Redaction: names, serials, titles and raw event XML were masked. target.dmp (if present) is NOT sanitised.\r\n", f);

    fputs("\r\nFiles\r\n-----\r\n"
          "summary.json         Machine-readable copy of this summary.\r\n"
          "timeline.csv         RAM history before the trigger (empty cell = counter unavailable).\r\n"
          "foreground.txt       Identity, elevation, bitness, uptime, window geometry, memory/I-O of the foreground app.\r\n"
          "processes.csv        All processes with CPU/I-O/page-fault rates over the post-trigger window.\r\n"
          "threads.csv          Foreground-process threads with names, priorities and CPU use.\r\n"
          "waitchains.txt       Wait Chain Traversal; shows what the UI thread was blocked on.\r\n"
          "modules.csv          DLLs loaded in the foreground process.\r\n"
          "target.dmp           Minidump of the foreground process (when permitted).\r\n"
          "events_digest.csv    Classified Event Log records with age relative to the trigger.\r\n"
          "events_*.xml         Raw Event Log XML (omitted with --redact).\r\n"
          "system_info.txt      OS build, hardware model, BIOS, memory, uptime, power source.\r\n"
          "power.txt            Power plan, per-CPU frequency limits, thermal zones.\r\n"
          "drives.csv           Volume capacity and free space.\r\n"
          "storage_devices.csv  Physical disks: bus, HDD/SSD, firmware.\r\n"
          "loaded_drivers.csv   Kernel drivers with vendor, version, date, third-party flag.\r\n"
          "displays.csv, display_drivers.csv  Display adapters and GPU driver versions.\r\n", f);

    fputs("\r\nHow to read this\r\n----------------\r\n"
          "Findings are heuristics: evidence to verify, not proof of root cause. Confidence reflects how directly the\r\n"
          "evidence ties to a stall, not how likely the component is to be at fault. A report with few findings does not\r\n"
          "mean the system is healthy. A complete hard lock prevents any user-mode tool from writing a report.\r\n", f);
    if (fclose(f) != 0) warn(c, "summary.txt may be incomplete (write error)");
}

static void write_summary_json(Cap *c, const Outcome *o) {
    WCHAR p[WFT_PATH_CCH]; wft_wsnprintf(p, WFT_PATH_CCH, L"%ls\\summary.json", c->dir);
    FILE *f = plat_fopen_w(p); if (!f) return;
    const Sample *t = o->trig;
    fputs("{\n  \"schema\": 1,\n  \"version\": \"" WFT_VERSION "\",\n  \"trigger\": ", f); wft_json_str(f, trigger_name(o->reason));
    fprintf(f, ",\n  \"time_local\": \"%04u-%02u-%02uT%02u:%02u:%02u.%03u\",\n  \"incident\": ", t->ts.year, t->ts.month, t->ts.day, t->ts.hour, t->ts.minute, t->ts.second, t->ts.millis);
    wft_json_str(f, o->incident);
    fprintf(f, ",\n  \"wake_late_ms\": %d,\n  \"stall_threshold_ms\": %lu,\n  \"foreground\": {\"pid\": %lu, \"tid\": %lu, \"exe\": ", t->wake_late_ms,
            (unsigned long)c->cfg->stall_ms, (unsigned long)o->fg->pid, (unsigned long)o->fg->tid);
    wft_json_wstr(f, wft_base_name(o->fg->path)); fputs(", \"probe\": ", f); wft_json_str(f, probe_status_name(o->fg->status));
    fputs("},\n  \"findings\": ", f); analysis_write_findings_json(f, o->an);
    fputs(",\n  \"metrics\": ", f); analysis_write_metrics_json(f, o->an);
    fprintf(f, ",\n  \"capture\": {\"total_ms\": %.0f, \"minidump\": ", o->total_ms); wft_json_str(f, dump_result_text(o->dump));
    fputs(", \"warnings\": [", f);
    for (int i = 0; i < c->nw; ++i) { if (i) fputc(',', f); wft_json_str(f, c->warn[i]); }
    fputs("]}\n}\n", f);
    fclose(f);
}

/* the capture itself */

bool capture_run(const Config *cfg, TriggerReason reason, const Sample *s, int n, const ForegroundInfo *fg,
                 bool resumed_recently, WCHAR *out_dir, size_t out_cch) {
    if (n <= 0) return false;
    double t_start = plat_qpc_ms();
    FILETIME trig_now; GetSystemTimeAsFileTime(&trig_now);       /* event ages are relative to this, not to the end of the minidump */
    ULONGLONG trigger_ft = ((ULONGLONG)trig_now.dwHighDateTime << 32) | trig_now.dwLowDateTime;
    static Cap c;                                   /* single-threaded; keeps ~5 KB off the stack */
    memset(&c, 0, sizeof c);
    c.cfg = cfg;
    const Sample *trig = &s[n - 1];

    report_prune(cfg->report_root, cfg->max_reports);
    if (!make_incident_dir(cfg, &trig->ts, reason, c.dir, WFT_PATH_CCH)) {
        log_msg_w("ERROR", "cannot create an incident folder under %s", cfg->report_root);
        return false;
    }
    wft_wsnprintf(out_dir, out_cch, L"%ls", c.dir);
    log_msg("INFO", "capture started: %s", trigger_name(reason));
    log_msg_w("INFO", "incident folder: %s", c.dir);
    uint64_t free_mb = 0;
    if (cfg->min_free_mb && plat_free_mb(c.dir, &free_mb) && free_mb < cfg->min_free_mb) warn(&c, "only %llu MB free on the report volume (--min-free-mb %lu)", (unsigned long long)free_mb, (unsigned long)cfg->min_free_mb);

    static Outcome o; memset(&o, 0, sizeof o);
    o.trig = trig; o.fg = fg; o.reason = reason; o.nsamples = n;
    ProcSnap ps; ThreadSnap ts;
    EventsResult *ev = (EventsResult *)calloc(1, sizeof *ev);
    AnalysisResult *an = (AnalysisResult *)calloc(1, sizeof *an);
    AnalysisInput *in = (AnalysisInput *)calloc(1, sizeof *in);
    if (!ev || !an || !in) { warn(&c, "out of memory"); free(ev); free(an); free(in); return false; }
    event_summary_init(&ev->summary);

    step_begin(&c, "baseline");   procsnap_begin(&ps); threads_begin(fg->pid, &ts); step_end(&c);
    step_begin(&c, "timeline");   if (!write_timeline(&c, s, n)) warn(&c, "timeline.csv could not be written"); step_end(&c);
    step_begin(&c, "foreground"); foreground_write_details(c.dir, fg, cfg); step_end(&c);
    step_begin(&c, "waitchains"); wait_chains_write(c.dir, &ts, fg->pid, fg->tid, 2500, &o.wi); step_end(&c);

    double el = plat_qpc_ms() - ps.base_ms;          /* give the CPU/I-O deltas a meaningful window */
    if (el < 400.0) Sleep((DWORD)(400.0 - el));
    step_begin(&c, "processes");
    procsnap_finish(&ps, fg->pid);
    o.proc_rows = ps.row_n; o.proc_window_ms = ps.base_ms;
    if (!procsnap_write_csv(c.dir, &ps, cfg)) warn(&c, "processes.csv could not be fully written");
    o.ntop_cpu = procsnap_top(&ps, 0, o.top_cpu, 5); o.ntop_io = procsnap_top(&ps, 1, o.top_io, 5);
    if (ps.truncated) warn(&c, "process list truncated");
    step_end(&c);
    step_begin(&c, "threads");    o.nhot = threads_finish_write(c.dir, fg->pid, fg->tid, &ts, o.hot, 5); if (ts.truncated) warn(&c, "thread list truncated"); step_end(&c);
    step_begin(&c, "modules");    modules_write(c.dir, fg->pid, cfg); step_end(&c);
    step_begin(&c, "minidump");
    if (reason == TRIGGER_STORAGE_STALL) { o.dump = DUMP_SKIPPED_STORAGE; o.dump_err = 0; o.dump_ms = 0; }   /* the app is not the cause, and the dump would be written to the stalled disk */
    else o.dump = dump_write(c.dir, fg->pid, fg->path, cfg, &o.dump_err, &o.dump_ms);
    step_end(&c);
    step_begin(&c, "events");     events_capture(c.dir, cfg, trigger_ft, ev); step_end(&c);
    DriveFacts df; PowerFacts pf;
    step_begin(&c, "system");     sysinfo_write_system(c.dir, cfg); sysinfo_write_drives(c.dir, &df); sysinfo_write_power(c.dir, &pf); step_end(&c);
    step_begin(&c, "drivers");    sysinfo_write_drivers(c.dir, cfg); step_end(&c);
    step_begin(&c, "hardware");   sysinfo_write_storage(c.dir, cfg); sysinfo_write_displays(c.dir, cfg); step_end(&c);
    step_begin(&c, "startup");    sysinfo_write_startup(c.dir, cfg); step_end(&c);

    SYSTEM_INFO si; GetSystemInfo(&si);
    in->reason = reason; in->s = s; in->n = n; in->stall_ms = cfg->stall_ms; in->logical_cpus = si.dwNumberOfProcessors; in->lead_seconds = cfg->lead_seconds;
    in->wait_cycle = o.wi.any_cycle; in->resumed_recently = resumed_recently;
    wft_snprintf(in->ui_blocker, sizeof in->ui_blocker, "%s", o.wi.ui_blocker); in->ui_cross_process = o.wi.ui_cross_process;
    in->ev = ev->summary;
    in->lowest_free_pct = df.lowest_free_pct; in->lowest_free_gb = df.lowest_free_gb; wft_snprintf(in->lowest_free_drive, sizeof in->lowest_free_drive, "%s", df.lowest_drive);
    in->max_temp_c = pf.max_temp_c; in->min_passive_limit_pct = pf.min_passive_pct;
    analysis_run(in, an);
    o.an = an; o.ev = ev; o.ts = &ts;
    const WCHAR *leaf = wft_base_name(c.dir); wft_ascii_from_wide(o.incident, sizeof o.incident, leaf);
    o.total_ms = plat_qpc_ms() - t_start;
    write_summary_txt(&c, &o);
    write_summary_json(&c, &o);

    log_msg("INFO", "capture finished in %.0f ms (%d finding(s), top: %s)", plat_qpc_ms() - t_start, an->count, an->count ? an->f[0].id : "none");
    procsnap_free(&ps); threads_free(&ts);
    free(ev); free(an); free(in);
    report_prune(cfg->report_root, cfg->max_reports);
    return true;
}
