/* Unit tests for the platform-independent core. Build: see CMakeLists.txt or tests/run.sh */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "eventclass.h"
#include "findings.h"
#include "retention.h"
#include "ring.h"
#include "stats.h"
#include "strutil.h"
#include "trigger.h"

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { ++g_run; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *slurp(FILE *f) {
    static char buf[16384];
    long n; fflush(f); rewind(f); n = (long)fread(buf, 1, sizeof buf - 1, f); buf[n < 0 ? 0 : n] = 0; return buf;
}

static void test_strutil(void) {
    uint32_t v = 0;
    CHECK(wft_parse_u32(L"1000", &v) && v == 1000);
    CHECK(!wft_parse_u32(L"", &v));
    CHECK(!wft_parse_u32(L"-5", &v));
    CHECK(!wft_parse_u32(L"12a", &v));
    CHECK(!wft_parse_u32(L" 12", &v));
    CHECK(!wft_parse_u32(L"4294967296", &v));
    CHECK(wft_parse_u32(L"4294967295", &v) && v == 4294967295u);

    wchar_t w[8]; int n = wft_wsnprintf(w, 4, L"%ls", L"abcdefgh");
    CHECK(n == 3 && w[3] == 0 && wcscmp(w, L"abc") == 0);
    char c[6]; wft_snprintf(c, sizeof c, "%s", "0123456789"); CHECK(strlen(c) == 5);

    char u[32];
    wft_wcs_to_utf8(L"A\u00e9\u20ac", u, sizeof u);
    CHECK(strcmp(u, "A\xC3\xA9\xE2\x82\xAC") == 0);
#if WCHAR_MAX > 0xFFFF
    wft_wcs_to_utf8((const wchar_t[]){ 0x1F600, 0 }, u, sizeof u);
    CHECK(strcmp(u, "\xF0\x9F\x98\x80") == 0);
#else
    wft_wcs_to_utf8((const wchar_t[]){ 0xD83D, 0xDE00, 0 }, u, sizeof u);   /* surrogate pair */
    CHECK(strcmp(u, "\xF0\x9F\x98\x80") == 0);
    wft_wcs_to_utf8((const wchar_t[]){ 0xD83D, 'x', 0 }, u, sizeof u);      /* lone high surrogate */
    CHECK(strcmp(u, "\xEF\xBF\xBD" "x") == 0);
#endif
    wft_wcs_to_utf8(L"\u20ac\u20ac\u20ac", u, 5);                          /* truncation on a char boundary */
    CHECK(strcmp(u, "\xE2\x82\xAC") == 0);

    FILE *f = tmpfile();
    wft_csv_text(f, L"=HYPERLINK(\"x\")"); CHECK(strcmp(slurp(f), "\"'=HYPERLINK(\"\"x\"\")\"") == 0);
    rewind(f); wft_csv_text(f, L"+1"); CHECK(strncmp(slurp(f), "\"'+1\"", 5) == 0);
    FILE *g = tmpfile();
    wft_csv_text(g, L"a\r\nb\tc"); CHECK(strcmp(slurp(g), "\"a  b c\"") == 0);
    FILE *h = tmpfile();
    wft_csv_text(h, L"plain, text"); CHECK(strcmp(slurp(h), "\"plain, text\"") == 0);
    FILE *j = tmpfile();
    wft_json_str(j, "q\"\\\n\x01"); CHECK(strcmp(slurp(j), "\"q\\\"\\\\\\n\\u0001\"") == 0);
    fclose(f); fclose(g); fclose(h); fclose(j);

    wchar_t p[64] = L"C:\\Users\\Bob\\AppData\\x.exe";
    wft_redact_user_path(p, 64);  CHECK(wcscmp(p, L"C:\\Users\\<user>\\AppData\\x.exe") == 0);
    wchar_t q[24] = L"C:\\users\\a\\b";
    wft_redact_user_path(q, 24);  CHECK(wcscmp(q, L"C:\\users\\<user>\\b") == 0);
    wchar_t t[24] = L"C:\\Users\\Bo\\verylong";          /* replacement is longer than the name: must truncate, not overflow */
    wft_redact_user_path(t, 24);  CHECK(wcslen(t) == 23 && wcsncmp(t, L"C:\\Users\\<user>\\", 15) == 0);
    CHECK(wcscmp(wft_base_name(L"C:\\a\\b\\game.exe"), L"game.exe") == 0);
    CHECK(wft_wcs_icontains(L"Microsoft-Windows-WHEA-Logger", L"whea"));
    CHECK(!wft_wcs_icontains(L"abc", L"abcd"));

    wchar_t qa[64];
    CHECK(wft_quote_arg(L"plain", qa, 64) && wcscmp(qa, L"plain") == 0);
    CHECK(wft_quote_arg(L"", qa, 64) && wcscmp(qa, L"\"\"") == 0);
    CHECK(wft_quote_arg(L"D:\\my reports", qa, 64) && wcscmp(qa, L"\"D:\\my reports\"") == 0);
    CHECK(wft_quote_arg(L"C:\\dir with space\\", qa, 64) && wcscmp(qa, L"\"C:\\dir with space\\\\\"") == 0);   /* trailing backslash doubled */
    CHECK(wft_quote_arg(L"say \"hi\"", qa, 64) && wcscmp(qa, L"\"say \\\"hi\\\"\"") == 0);
    CHECK(wft_quote_arg(L"a\\\"b c", qa, 64) && wcscmp(qa, L"\"a\\\\\\\"b c\"") == 0);                   /* backslash before a quote */
    CHECK(!wft_quote_arg(L"this does not fit", qa, 8) && qa[0] == 0);
    CHECK(!wft_quote_arg(L"abcdefgh", qa, 8));                                                      /* needs 9 */
}

static void test_config(void) {
    Config c; char err[128];
    config_defaults(&c);
    wchar_t *ok1[] = { L"x", L"--interval", L"500", L"--no-dump", L"--stall-ms=2000", L"--report-dir", L"D:\\r" };
    CHECK(config_parse(&c, 7, ok1, err, sizeof err) == CLI_OK);
    CHECK(c.interval_ms == 500 && !c.make_dump && c.stall_ms == 2000 && c.report_dir_set);

    config_defaults(&c);
    wchar_t *bad1[] = { L"x", L"--bogus" };            CHECK(config_parse(&c, 2, bad1, err, sizeof err) == CLI_ERROR && strstr(err, "bogus"));
    wchar_t *bad2[] = { L"x", L"--interval" };         CHECK(config_parse(&c, 2, bad2, err, sizeof err) == CLI_ERROR);
    wchar_t *bad3[] = { L"x", L"--interval", L"abc" }; CHECK(config_parse(&c, 3, bad3, err, sizeof err) == CLI_ERROR);
    wchar_t *bad4[] = { L"x", L"--report-dir", L"a|b" };CHECK(config_parse(&c, 3, bad4, err, sizeof err) == CLI_ERROR);
    wchar_t *help[] = { L"x", L"--help" };             CHECK(config_parse(&c, 2, help, err, sizeof err) == CLI_HELP);
    wchar_t *ver[]  = { L"x", L"--version" };          CHECK(config_parse(&c, 2, ver, err, sizeof err) == CLI_VERSION);

    config_defaults(&c);
    wchar_t *clampme[] = { L"x", L"--interval", L"5", L"--hang-timeout-ms", L"0", L"--hang-samples", L"1", L"--cooldown", L"0" };
    CHECK(config_parse(&c, 9, clampme, err, sizeof err) == CLI_OK);
    CHECK(c.interval_ms == 250 && c.hang_timeout_ms == 50 && c.hang_samples == 2 && c.cooldown_seconds == 5);
    CHECK(c.warning_count >= 4);

    config_defaults(&c);
    wchar_t *cap[] = { L"x", L"--interval", L"250", L"--history", L"600" };
    config_parse(&c, 5, cap, err, sizeof err);
    CHECK(config_ring_capacity(&c) == WFT_MAX_RING && c.warning_count == 1);
    config_defaults(&c); CHECK(config_ring_capacity(&c) == 180);
    CHECK(sizeof(Sample) * 180 < 1024 * 1024);                      /* README promise: < 1 MiB */

    /* 0.6.0: tray and start-up moved to WinFreezeTrace Manager */
    config_defaults(&c); CHECK(!c.exit_running);
    wchar_t *t1[] = { L"x", L"--no-tray", L"--no-notify", L"--background" };          /* old start-up entries must keep working */
    CHECK(config_parse(&c, 4, t1, err, sizeof err) == CLI_OK);
    config_defaults(&c);
    wchar_t *t2[] = { L"x", L"--add-startup", L"--redact" };
    CHECK(config_parse(&c, 3, t2, err, sizeof err) == CLI_ERROR && strstr(err, "Manager"));
    config_defaults(&c);
    wchar_t *t3[] = { L"x", L"--remove-startup" };
    CHECK(config_parse(&c, 2, t3, err, sizeof err) == CLI_ERROR && strstr(err, "Manager"));
    config_defaults(&c);
    wchar_t *t5[] = { L"x", L"--exit" };
    CHECK(config_parse(&c, 2, t5, err, sizeof err) == CLI_OK && c.exit_running);

    /* the Manager reproduces the configuration from these arguments, never the one-shot commands */
    wchar_t args[512];
    config_defaults(&c); CHECK(config_format_args(&c, args, 512) && args[0] == 0);
    wchar_t *t6[] = { L"x", L"--exit", L"--interval", L"2000", L"--no-dump", L"--redact", L"--no-tray", L"--report-dir", L"D:\\my reports" };
    CHECK(config_parse(&c, 9, t6, err, sizeof err) == CLI_OK);
    CHECK(config_format_args(&c, args, 512));
    CHECK(wcscmp(args, L"--interval 2000 --no-dump --redact --report-dir \"D:\\my reports\"") == 0);
    CHECK(!config_format_args(&c, args, 20) && args[0] == 0);
    CHECK(wcsstr(args, L"exit") == NULL);
}

static void test_ring(void) {
    SampleRing r; CHECK(ring_init(&r, 5));
    Sample s; memset(&s, 0, sizeof s);
    for (int i = 0; i < 12; ++i) { s.wake_late_ms = i; ring_push(&r, &s); }
    Sample out[8]; int n = ring_linearize(&r, out, 8);
    CHECK(n == 5 && out[0].wake_late_ms == 7 && out[4].wake_late_ms == 11);
    n = ring_linearize(&r, out, 3); CHECK(n == 3 && out[0].wake_late_ms == 9 && out[2].wake_late_ms == 11);
    ring_reset(&r); CHECK(ring_linearize(&r, out, 8) == 0);                 /* pause/resume must not leave a gap in the history */
    s.wake_late_ms = 42; ring_push(&r, &s); n = ring_linearize(&r, out, 8); CHECK(n == 1 && out[0].wake_late_ms == 42);
    ring_free(&r);
}

static Sample mk(double t_ms) {
    Sample s; memset(&s, 0, sizeof s);
    metrics_reset_na(&s.m);
    s.mono_ms = t_ms; s.probe_ms = 1.0; s.probe_status = PROBE_OK;
    s.m.cpu = 10; s.m.disk_latency_ms = 2; s.m.disk_worst_latency_ms = 3; s.m.commit_pct = 40; s.m.mem_load = 50; s.m.avail_mb = 8000;
    return s;
}

static void test_stats(void) {
    Sample a[10];
    for (int i = 0; i < 10; ++i) { a[i] = mk(i * 1000.0); a[i].m.cpu = (double)(i + 1); }
    a[3].m.cpu = WFT_NA;
    WindowStat w; stats_window(a, 0, 10, WFT_METRICS[MI_CPU].fn, 9000.0, &w);
    CHECK(w.n == 9 && w.max == 10 && w.min == 1 && w.max_age_s == 0.0 && w.min_age_s == 9.0);
    CHECK(w.median == 6 && w.p95 == 10 && w.last == 10);
    stats_window(a, 0, 0, WFT_METRICS[MI_CPU].fn, 0, &w); CHECK(w.n == 0 && !WFT_HAS(w.max));
}

static void test_trigger(void) {
    TriggerConfig c; TriggerState s; trigger_config_default(&c, 1500, 3, 60); trigger_init(&s);
    TriggerInput in = { 100000.0, 0, PROBE_OK, false };
    CHECK(trigger_evaluate(&s, &c, &in).reason == TRIGGER_NONE);
    in.late_ms = 2000; TriggerDecision d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_SCHEDULER_STALL);
    trigger_note_capture(&s, in.now_ms);
    in.now_ms += 5000; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_NONE && d.suppressed && !strcmp(d.suppressed, "cooldown"));
    in.manual = true; in.late_ms = 0; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_MANUAL);                               /* manual bypasses cooldown (bug in 0.3.0) */
    in.now_ms += 500; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_NONE && d.suppressed);                  /* but is rate limited */
    in.manual = false;

    trigger_init(&s);
    in.now_ms = 500000; in.late_ms = 70000; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_NONE && !strcmp(d.suppressed, "sleep-or-hibernate-gap"));
    trigger_note_resume(&s, &c, in.now_ms);
    in.now_ms += 1000; in.late_ms = 3000; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_NONE && !strcmp(d.suppressed, "resume-grace"));
    in.now_ms += 20000; d = trigger_evaluate(&s, &c, &in);
    CHECK(d.reason == TRIGGER_SCHEDULER_STALL);

    trigger_init(&s); in.late_ms = 0; in.probe_status = PROBE_TIMEOUT; in.now_ms = 1e6;
    CHECK(trigger_evaluate(&s, &c, &in).reason == TRIGGER_NONE);
    CHECK(trigger_evaluate(&s, &c, &in).reason == TRIGGER_NONE);
    CHECK(trigger_evaluate(&s, &c, &in).reason == TRIGGER_APP_HANG);
    trigger_init(&s);
    in.probe_status = PROBE_TIMEOUT; trigger_evaluate(&s, &c, &in); trigger_evaluate(&s, &c, &in);
    in.probe_status = PROBE_DENIED; trigger_evaluate(&s, &c, &in);   /* UIPI denial must reset the streak */
    in.probe_status = PROBE_TIMEOUT;
    CHECK(trigger_evaluate(&s, &c, &in).reason == TRIGGER_NONE);
}

static void test_late(void) {
    double slept = -1;
    CHECK(trigger_late_ms(1000.0, 1000.0, 1000, &slept) == 0 && slept == 0.0);
    CHECK(trigger_late_ms(1250.0, 1250.0, 1000, &slept) == 250 && slept == 0.0);        /* a real stall: both clocks advance */
    CHECK(trigger_late_ms(980.0, 980.0, 1000, &slept) == 0);                            /* early wake-up never goes negative */
    /* 30 s of sleep (shorter than the 60 s gap heuristic) is removed, not reported as a stall */
    CHECK(trigger_late_ms(31000.0, 1000.0, 1000, &slept) == 0 && slept == 30000.0);
    /* ...but a stall that happens around it is still seen */
    CHECK(trigger_late_ms(33800.0, 2000.0, 1000, &slept) == 1000 && slept == 31800.0);
    CHECK(trigger_late_ms(5000.0, -1.0, 1000, &slept) == 4000 && slept == 0.0);         /* unbiased clock unavailable */
    CHECK(trigger_late_ms(1500.0, 1000.0, 1000, &slept) == 500 && slept == 0.0);        /* under 1 s of difference is noise, not sleep */
    CHECK(trigger_late_ms(1e15, 1000.0, 1000, &slept) >= 0);                            /* multi-week gap: no int overflow */
    CHECK(trigger_late_ms(1e15, -1.0, 1000, NULL) == 2147483647);
}

static void test_events_and_retention(void) {
    CHECK(event_classify(L"Microsoft-Windows-WHEA-Logger", 18, 1, false) == EVC_WHEA_UNCORRECTED);
    CHECK(event_classify(L"Microsoft-Windows-WHEA-Logger", 17, 3, false) == EVC_WHEA_CORRECTED);
    CHECK(event_classify(L"Microsoft-Windows-WHEA-Logger", 17, 0, false) == EVC_WHEA_CORRECTED);   /* unknown level is not "uncorrected" */
    CHECK(event_classify(L"Display", 4101, 3, false) == EVC_DISPLAY);
    CHECK(event_classify(L"nvlddmkm", 14, 2, false) == EVC_DISPLAY);
    CHECK(event_classify(L"disk", 153, 3, false) == EVC_STORAGE);
    CHECK(event_classify(L"Microsoft-Windows-Ntfs", 55, 2, false) == EVC_FILESYSTEM);
    CHECK(event_classify(L"Windows Error Reporting", 1001, 4, true) == EVC_LIVEKERNEL);
    CHECK(event_classify(L"Windows Error Reporting", 1001, 4, false) == EVC_NONE);
    CHECK(event_classify(L"Microsoft-Windows-Kernel-Power", 41, 1, false) == EVC_UNEXPECTED_SHUTDOWN);
    CHECK(event_classify(L"Microsoft-Windows-Kernel-Power", 107, 4, false) == EVC_SLEEP_RESUME);
    CHECK(event_classify(L"Microsoft-Windows-Kernel-Processor-Power", 37, 3, false) == EVC_THROTTLE);
    CHECK(event_classify(L"Some DisplayName Helper", 1, 2, false) == EVC_NONE);    /* old substring scan matched this */
    EventSummary es; event_summary_init(&es);
    event_summary_add(&es, EVC_DISPLAY, 10.0, WFT_NEAR_EVENT_S, "Display 4101");
    event_summary_add(&es, EVC_DISPLAY, 900.0, WFT_NEAR_EVENT_S, "Display 4101");
    CHECK(es.cat[EVC_DISPLAY].total == 2 && es.cat[EVC_DISPLAY].near_count == 1 && es.cat[EVC_DISPLAY].nearest_age_s == 10.0);

    CHECK(retention_dir_name_ok(L"20261022_164930_scheduler_stall"));
    CHECK(retention_dir_name_ok(L"20261022_164930_manual_hotkey_2"));
    CHECK(retention_dir_name_ok(L"20261022_164930"));
    CHECK(!retention_dir_name_ok(L"Documents"));
    CHECK(!retention_dir_name_ok(L"20261022_164930_bad..name"));
    CHECK(!retention_dir_name_ok(L"20261022-164930_x"));
    CHECK(retention_file_name_ok(L"target.dmp") && retention_file_name_ok(L"Summary.TXT") && !retention_file_name_ok(L"x.exe"));
    const wchar_t *names[] = { L"20260103_000000_a", L"keepme", L"20260101_000000_a", L"20260102_000000_a", L"20260104_000000_a" };
    int del[8]; int k = retention_select(names, 5, 2, del);
    CHECK(k == 2 && wcscmp(names[del[0]], L"20260101_000000_a") == 0 && wcscmp(names[del[1]], L"20260102_000000_a") == 0);
    CHECK(retention_select(names, 5, 0, del) == 0);
}

static void fill(Sample *a, int n) { for (int i = 0; i < n; ++i) a[i] = mk(i * 1000.0); }
static bool has_id(const AnalysisResult *r, const char *id) { for (int i = 0; i < r->count; ++i) if (!strcmp(r->f[i].id, id)) return true; return false; }
static AnalysisInput base_in(Sample *a, int n, TriggerReason why) {
    AnalysisInput in; memset(&in, 0, sizeof in);
    in.reason = why; in.s = a; in.n = n; in.stall_ms = 1500; in.logical_cpus = 8; in.lead_seconds = 30;
    in.lowest_free_pct = in.lowest_free_gb = in.max_temp_c = in.min_passive_limit_pct = WFT_NA;
    event_summary_init(&in.ev);
    return in;
}

static void test_analysis(void) {
    static Sample a[120]; static AnalysisResult r;
    fill(a, 120);
    a[116].m.disk_worst_latency_ms = 450.0; strcpy(a[116].m.disk_worst_name, "1 D:");
    a[119].wake_late_ms = 2200;
    AnalysisInput in = base_in(a, 120, TRIGGER_SCHEDULER_STALL);
    analysis_run(&in, &r);
    CHECK(has_id(&r, "disk_latency") && !has_id(&r, "stall_unexplained"));
    for (int i = 0; i < r.count; ++i) if (!strcmp(r.f[i].id, "disk_latency")) {
        CHECK(r.f[i].conf == CONF_MED && strstr(r.f[i].evidence, "t-3s") && strstr(r.f[i].evidence, "1 D:"));
    }

    /* The same spike 100 s before the trigger belongs to the baseline, not the lead-in. */
    fill(a, 120); a[19].m.disk_worst_latency_ms = 450.0; a[119].wake_late_ms = 2200;
    in = base_in(a, 120, TRIGGER_SCHEDULER_STALL); analysis_run(&in, &r);
    CHECK(!has_id(&r, "disk_latency") && has_id(&r, "stall_unexplained"));

    fill(a, 120); in = base_in(a, 120, TRIGGER_APP_HANG);
    in.wait_cycle = true; analysis_run(&in, &r);
    CHECK(r.f[0].conf == CONF_HIGH && !strcmp(r.f[0].id, "wait_cycle") && has_id(&r, "app_specific_hang"));

    fill(a, 120); in = base_in(a, 120, TRIGGER_SCHEDULER_STALL);
    event_summary_add(&in.ev, EVC_DISPLAY, 5.0, WFT_NEAR_EVENT_S, "Display 4101");
    event_summary_add(&in.ev, EVC_WHEA_CORRECTED, 1500.0, WFT_NEAR_EVENT_S, "WHEA 17");
    analysis_run(&in, &r);
    CHECK(r.f[0].conf == CONF_HIGH && !strcmp(r.f[0].id, "display_driver"));
    for (int i = 0; i < r.count; ++i) if (!strcmp(r.f[i].id, "whea_corrected")) CHECK(r.f[i].conf == CONF_LOW);   /* old event: only LOW */

    fill(a, 120); in = base_in(a, 120, TRIGGER_SCHEDULER_STALL); in.resumed_recently = true; analysis_run(&in, &r);
    CHECK(has_id(&r, "sleep_resume"));

    fill(a, 120); a[118].m.cpu = 99; a[118].m.proc_queue = 20; a[118].m.core_dpc_max = 55; strcpy(a[118].m.core_dpc_name, "0,3");
    in = base_in(a, 120, TRIGGER_MANUAL); analysis_run(&in, &r);
    CHECK(has_id(&r, "cpu_saturation") && has_id(&r, "dpc_interrupt") && has_id(&r, "manual"));

    /* Unavailable counters must never produce findings (they were shown as 0.0 before). */
    for (int i = 0; i < 120; ++i) metrics_reset_na(&a[i].m);
    in = base_in(a, 120, TRIGGER_MANUAL); analysis_run(&in, &r);
    CHECK(r.count == 1 && has_id(&r, "manual"));

    in = base_in(a, 0, TRIGGER_MANUAL); analysis_run(&in, &r);                  /* empty timeline: no crash, one explanatory finding */
    CHECK(r.count == 1 && has_id(&r, "no_data"));
    in = base_in(a, 120, TRIGGER_MANUAL); analysis_run(&in, &r);

    FILE *f = tmpfile();
    analysis_write_findings_text(f, &r); analysis_write_metric_table(f, &r, 30);
    analysis_write_event_summary(f, &in.ev); analysis_write_findings_json(f, &r); analysis_write_metrics_json(f, &r);
    CHECK(strstr(slurp(f), "n/a") != NULL);
    fclose(f);
}

int main(void) {
    test_strutil(); test_config(); test_ring(); test_stats(); test_trigger(); test_late(); test_events_and_retention(); test_analysis();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
