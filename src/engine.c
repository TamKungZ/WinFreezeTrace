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
#include "win/ui.h"

#include "ring.h"
#include "trigger.h"

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    log_msg("FATAL", "unhandled exception 0x%08lx at %p; exiting so the scheduled task can restart the recorder",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void log_config(const Config *c, const PdhState *p, bool ui_ok) {
    log_msg("INFO", "WinFreezeTrace %s started: interval=%lu ms history=%lu s stall=%lu ms hang=%lu ms x%lu cooldown=%lu s dump=%s redact=%s max-reports=%lu",
            WFT_VERSION, (unsigned long)c->interval_ms, (unsigned long)c->history_seconds, (unsigned long)c->stall_ms, (unsigned long)c->hang_timeout_ms,
            (unsigned long)c->hang_samples, (unsigned long)c->cooldown_seconds, c->make_dump ? "on" : "off", c->redact ? "on" : "off", (unsigned long)c->max_reports);
    log_msg_w("INFO", "report root: %s", c->report_root);
    for (int i = 0; i < c->warning_count; ++i) log_msg("WARN", "config: %s", c->warnings[i]);
    if (!p->ok) log_msg("WARN", "PDH unavailable: only memory/system metrics will be recorded");
    else if (p->missing) log_msg("WARN", "%d performance counter(s) are unavailable on this system (their columns will be empty)", p->missing);
    if (!ui_ok) log_msg("WARN", "no hidden window: hotkey and sleep/resume detection are disabled");
}

const char *WftVersion(void) { return WFT_VERSION; }

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

    if (!cfg.report_dir_set) wft_wsnprintf(cfg.report_root, WFT_PATH_CCH, L"%ls", default_reports);
    else {
        WCHAR abs[WFT_PATH_CCH];
        if (!plat_abs_path(cfg.report_root, abs, WFT_PATH_CCH)) { log_msg("ERROR", "invalid --report-dir"); return 64; }
        wft_wsnprintf(cfg.report_root, WFT_PATH_CCH, L"%ls", abs);
    }
    if (plat_create_private_dir(cfg.report_root) == DIR_FAILED)
        log_msg_w("ERROR", "cannot create report folder %s; captures will fail until it exists", cfg.report_root);

    HANDLE singleton = CreateMutexW(NULL, TRUE, L"Local\\WinFreezeTrace.Singleton.v2");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { log_msg("INFO", "another instance is already running in this session; exiting"); return 2; }

    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    SampleRing ring; int cap = config_ring_capacity(&cfg);
    Sample *lin = (Sample *)calloc((size_t)cap, sizeof(Sample));
    if (!stop || !lin || !ring_init(&ring, cap)) { log_msg("FATAL", "out of memory"); return 3; }

    PdhState pdh; metrics_init(&pdh);
    bool ui_ok = ui_start(inst, stop, cfg.hotkey);
    log_config(&cfg, &pdh, ui_ok);

    TriggerConfig tc; trigger_config_default(&tc, cfg.stall_ms, cfg.hang_samples, cfg.cooldown_seconds);
    TriggerState ts; trigger_init(&ts);
    const DWORD self = plat_self_pid();
    double last_suppress_log = -1e12;

    for (;;) {
        double t0 = plat_qpc_ms();
        if (WaitForSingleObject(stop, cfg.interval_ms) == WAIT_OBJECT_0) break;
        double wake = plat_qpc_ms();
        int late = (int)((wake - t0) - (double)cfg.interval_ms);
        if (late < 0) late = 0;

        Sample s; memset(&s, 0, sizeof s);
        plat_local_time(&s.ts);
        s.mono_ms = wake; s.wake_late_ms = late;
        metrics_collect(&pdh, &s.m);
        ForegroundInfo fi; foreground_probe(&fi, cfg.hang_timeout_ms, self);
        foreground_to_sample(&fi, &s);
        ring_push(&ring, &s);

        if (ui_take_resume()) trigger_note_resume(&ts, &tc, wake);
        TriggerInput ti; ti.now_ms = wake; ti.late_ms = late; ti.probe_status = fi.status; ti.manual = ui_take_manual();
        TriggerDecision d = trigger_evaluate(&ts, &tc, &ti);

        if (d.suppressed && wake - last_suppress_log > 30000.0) {
            log_msg("INFO", "trigger ignored (%s): wake lateness %d ms, probe %s", d.suppressed, late, probe_status_name(fi.status));
            last_suppress_log = wake;
        }
        if (d.reason != TRIGGER_NONE) {
            int n = ring_linearize(&ring, lin, cap);
            WCHAR dir[WFT_PATH_CCH];
            bool resumed_recently = wake < ts.resume_until_ms;
            if (!capture_run(&cfg, d.reason, lin, n, &fi, resumed_recently, dir, WFT_PATH_CCH))
                log_msg("ERROR", "capture failed for trigger %s", trigger_name(d.reason));
            trigger_note_capture(&ts, plat_qpc_ms());
        }
    }

    log_msg("INFO", "shutting down");
    ui_stop();
    metrics_shutdown(&pdh);
    ring_free(&ring);
    free(lin);
    CloseHandle(stop);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
