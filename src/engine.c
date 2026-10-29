/*
 * WinFreezeTrace - engine.c
 *
 * Start-up, the sampling loop and shutdown (WftRun). Everything else lives in:
 *   core/  platform-independent logic (config, ring, stats, triggers, findings), unit-tested
 *   win/   Windows API wrappers (PDH, Event Log, WCT, minidump, ...)
 */
#include "wft_api.h"
#include "win/platform.h"
#include "win/metrics.h"
#include "win/foreground.h"
#include "win/report.h"
#include "win/hostwin.h"
#include "win/control.h"

#include "ring.h"
#include "trigger.h"

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    log_msg("FATAL", "unhandled exception 0x%08lx at %p; exiting so the scheduled task can restart the recorder",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void log_config(const Config *c, const PdhState *p, bool host_ok, bool control_ok) {
    log_msg("INFO", "WinFreezeTrace %s started: interval=%lu ms history=%lu s stall=%lu ms hang=%lu ms x%lu cooldown=%lu s dump=%s redact=%s max-reports=%lu",
            WFT_VERSION, (unsigned long)c->interval_ms, (unsigned long)c->history_seconds, (unsigned long)c->stall_ms, (unsigned long)c->hang_timeout_ms,
            (unsigned long)c->hang_samples, (unsigned long)c->cooldown_seconds, c->make_dump ? "on" : "off", c->redact ? "on" : "off", (unsigned long)c->max_reports);
    log_msg("INFO", "storage triggers: worst-disk latency >= %lu ms, queue >= %lu (0 = off)", (unsigned long)c->disk_latency_ms, (unsigned long)c->disk_queue);
    log_msg_w("INFO", "report root: %s", c->report_root);
    for (int i = 0; i < c->warning_count; ++i) log_msg("WARN", "config: %s", c->warnings[i]);
    if (!p->ok) log_msg("WARN", "PDH unavailable: only memory/system metrics will be recorded");
    else if (p->missing) log_msg("WARN", "%d performance counter(s) are unavailable on this system (their columns will be empty)", p->missing);
    if (!host_ok) log_msg("WARN", "no hidden window: hotkey and sleep/resume notifications are disabled");
    if (!control_ok) log_msg("WARN", "no control pipe: WinFreezeTrace Manager cannot show the state or switch monitoring on/off");
}

const char *WftVersion(void) { return WFT_VERSION; }

#define SINGLETON_NAME L"Local\\WinFreezeTrace.Singleton.v2"

static void say(const char *text) { plat_print(text); }

int WftRun(HINSTANCE inst) {
    plat_init();
    plat_harden_process();

    WCHAR app_root[WFT_PATH_CCH], default_reports[WFT_PATH_CCH];
    plat_default_paths(app_root, WFT_PATH_CCH, default_reports, WFT_PATH_CCH);
    log_init(app_root);
    SetUnhandledExceptionFilter(crash_filter);

    Config cfg; config_defaults(&cfg);
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char err[240] = "";
    CliResult cr = argv ? config_parse(&cfg, argc, argv, err, sizeof err) : CLI_OK;
    if (argv) LocalFree(argv);
    if (cr == CLI_HELP)    { plat_print(config_usage()); return 0; }
    if (cr == CLI_VERSION) { plat_print("WinFreezeTrace " WFT_VERSION "\n"); return 0; }
    if (cr == CLI_ERROR) {
        char msg[300]; wft_snprintf(msg, sizeof msg, "WinFreezeTrace: %s\n", err);
        plat_print(msg);
        log_msg("ERROR", "bad command line: %s", err);
        return 64;
    }

    /* --exit: politely ask the running instance of this session to quit (same as the Manager's Exit). */
    if (cfg.exit_running) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, SINGLETON_NAME);
        if (!m) { say("WinFreezeTrace is not running in this session.\n"); return 0; }
        CloseHandle(m);
        host_request_running_instance_exit();
        for (int i = 0; i < 100; ++i) {                  /* wait up to ~10 s (a capture may be in progress) */
            Sleep(100);
            m = OpenMutexW(SYNCHRONIZE, FALSE, SINGLETON_NAME);
            if (!m) { say("WinFreezeTrace stopped.\n"); return 0; }
            CloseHandle(m);
        }
        say("WinFreezeTrace did not stop within 10 seconds.\n");
        return 1;
    }

    /* Resolve the report folder (no directory is created yet). */
    if (!cfg.report_dir_set) wft_wsnprintf(cfg.report_root, WFT_PATH_CCH, L"%ls", default_reports);
    else {
        WCHAR abs[WFT_PATH_CCH];
        if (!plat_abs_path(cfg.report_root, abs, WFT_PATH_CCH)) { log_msg("ERROR", "invalid --report-dir"); say("WinFreezeTrace: invalid --report-dir\n"); return 64; }
        wft_wsnprintf(cfg.report_root, WFT_PATH_CCH, L"%ls", abs);
    }

    HANDLE singleton = CreateMutexW(NULL, TRUE, SINGLETON_NAME);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        log_msg("INFO", "another instance is already running in this session; exiting");
        say("WinFreezeTrace is already running in this session.\n");
        if (singleton) CloseHandle(singleton);
        return 2;
    }

    if (plat_create_private_dir_tree(cfg.report_root) == DIR_FAILED)
        log_msg_w("ERROR", "cannot create report folder %s; captures will fail until it exists", cfg.report_root);

    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE wake = CreateEventW(NULL, FALSE, FALSE, NULL);          /* auto-reset: hotkey / control pipe */
    SampleRing ring; int cap = config_ring_capacity(&cfg);
    Sample *lin = (Sample *)calloc((size_t)cap, sizeof(Sample));
    if (!stop || !wake || !lin || !ring_init(&ring, cap)) { log_msg("FATAL", "out of memory"); return 3; }

    PdhState pdh; metrics_init(&pdh);
    ControlOptions co; memset(&co, 0, sizeof co);
    co.reports_dir = cfg.report_root;
    bool control_ok = control_start(stop, wake, &co);
    HostOptions ho; memset(&ho, 0, sizeof ho);
    ho.hotkey = cfg.hotkey;
    bool host_ok = host_start(inst, stop, &ho);
    log_config(&cfg, &pdh, host_ok, control_ok);

    TriggerConfig tc; trigger_config_default(&tc, cfg.stall_ms, cfg.hang_samples, cfg.cooldown_seconds);
    tc.disk_latency_ms = (double)cfg.disk_latency_ms;
    tc.disk_queue = (double)cfg.disk_queue;
    TriggerState ts; trigger_init(&ts);
    const DWORD self = plat_self_pid();
    double last_suppress_log = -1e12, last_block_log = -1e12;
    bool was_paused = false;
    HANDLE waits[2] = { stop, wake };

    for (;;) {
        if (control_paused()) {                          /* switched off by the Manager: no sampling, no probing, no triggers */
            if (!was_paused) {
                was_paused = true;
                ring_reset(&ring);                       /* a gap would corrupt the baseline when monitoring resumes */
                trigger_init(&ts);
            }
            control_take_capture(); host_take_resume();
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }
        if (was_paused) { was_paused = false; log_msg("INFO", "monitoring resumed"); }

        double t0 = plat_qpc_ms(), u0 = plat_unbiased_ms();
        if (WaitForMultipleObjects(2, waits, FALSE, cfg.interval_ms) == WAIT_OBJECT_0) break;
        double wake_ms = plat_qpc_ms(), u1 = plat_unbiased_ms();
        double slept = 0.0;
        int late = trigger_late_ms(wake_ms - t0, (u0 >= 0.0 && u1 >= 0.0) ? u1 - u0 : -1.0, cfg.interval_ms, &slept);
        if (slept > 0.0) {                               /* detected even when no power broadcast reached us */
            log_msg("INFO", "system slept for about %.0f s; treating the wake-up as a resume", slept / 1000.0);
            trigger_note_resume(&ts, &tc, wake_ms);
        }
        if (control_paused()) continue;                  /* toggled off during the wait */

        Sample s; memset(&s, 0, sizeof s);
        plat_local_time(&s.ts);
        s.mono_ms = wake_ms;
        metrics_collect(&pdh, &s.m);
        double c1 = plat_qpc_ms();
        ForegroundInfo fi; foreground_probe(&fi, cfg.hang_timeout_ms, self);
        double c2 = plat_qpc_ms(), u2 = plat_unbiased_ms();

        /* The wait above only measures how late the wake-up was. If this thread then sits inside a PDH call, the
         * process snapshot or the probe (typical when storage or a driver stalls), that time would be invisible, so it
         * counts too. Sleep during the collection is removed the same way as for the wait. */
        double slept_in_work = 0.0;
        int work_late = trigger_late_ms(c2 - wake_ms, (u1 >= 0.0 && u2 >= 0.0) ? u2 - u1 : -1.0, 0, &slept_in_work);
        if (slept_in_work > 0.0) trigger_note_resume(&ts, &tc, c2);
        long long total_late = (long long)late + (long long)work_late;
        late = total_late > 2147483647LL ? 2147483647 : (int)total_late;
        s.wake_late_ms = late;
        if (work_late >= 500 && c2 - last_block_log > 10000.0) {
            log_msg("INFO", "sampling was blocked for %d ms (performance counters %.0f ms, foreground probe %.0f ms)",
                    work_late, c1 - wake_ms, c2 - c1);
            last_block_log = c2;
        }
        foreground_to_sample(&fi, &s);
        ring_push(&ring, &s);

        if (host_take_resume()) trigger_note_resume(&ts, &tc, wake_ms);
        TriggerInput ti; ti.now_ms = wake_ms; ti.late_ms = late; ti.probe_status = fi.status; ti.manual = control_take_capture();
        ti.disk_latency_ms = s.m.disk_worst_latency_ms; ti.disk_queue = s.m.disk_worst_queue;
        TriggerDecision d = trigger_evaluate(&ts, &tc, &ti);

        if (d.suppressed && wake_ms - last_suppress_log > 30000.0) {
            log_msg("INFO", "trigger ignored (%s): wake lateness %d ms, probe %s", d.suppressed, late, probe_status_name(fi.status));
            last_suppress_log = wake_ms;
        }
        if (d.reason != TRIGGER_NONE) {
            int n = ring_linearize(&ring, lin, cap);
            WCHAR dir[WFT_PATH_CCH]; dir[0] = 0;
            bool resumed_recently = wake_ms < ts.resume_until_ms;
            control_set_capturing(true);
            bool ok = capture_run(&cfg, d.reason, lin, n, &fi, resumed_recently, dir, WFT_PATH_CCH);
            control_set_capturing(false);
            if (!ok) log_msg("ERROR", "capture failed for trigger %s", trigger_name(d.reason));
            control_capture_done(dir, ok);
            trigger_note_capture(&ts, plat_qpc_ms());
        }
    }

    log_msg("INFO", "shutting down");
    host_stop();
    control_stop();
    metrics_shutdown(&pdh);
    ring_free(&ring);
    free(lin);
    CloseHandle(stop);
    CloseHandle(wake);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
