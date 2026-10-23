#include "config.h"
#include "strutil.h"

#include <string.h>

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    c->interval_ms = 1000;
    c->history_seconds = 180;
    c->stall_ms = 1500;
    c->hang_timeout_ms = 250;
    c->hang_samples = 3;
    c->cooldown_seconds = 60;
    c->lead_seconds = 30;
    c->max_reports = 50;
    c->min_free_mb = 1024;
    c->dump_timeout_ms = 15000;
    c->make_dump = true;
    c->hotkey = true;
}

static void warn(Config *c, const char *fmt, const char *name, uint32_t was, uint32_t now) {
    if (c->warning_count < 8)
        wft_snprintf(c->warnings[c->warning_count++], sizeof c->warnings[0], fmt, name, (unsigned)was, (unsigned)now);
}

static void clamp(Config *c, const char *name, uint32_t *v, uint32_t lo, uint32_t hi) {
    uint32_t was = *v;
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    if (*v != was) warn(c, "%s=%u is outside the supported range; using %u", name, was, *v);
}

typedef struct Opt { const wchar_t *name; const char *label; size_t off; uint32_t lo, hi; } Opt;
#define U32(f) offsetof(Config, f)

static const Opt k_opts[] = {
    { L"--interval",        "--interval",        U32(interval_ms),      250, 60000 },
    { L"--history",         "--history",         U32(history_seconds),   30,   600 },
    { L"--stall-ms",        "--stall-ms",        U32(stall_ms),         500, 30000 },
    { L"--hang-timeout-ms", "--hang-timeout-ms", U32(hang_timeout_ms),   50,  5000 },
    { L"--hang-samples",    "--hang-samples",    U32(hang_samples),       2,    20 },
    { L"--cooldown",        "--cooldown",        U32(cooldown_seconds),   5, 86400 },
    { L"--lead-window",     "--lead-window",     U32(lead_seconds),       5,   300 },
    { L"--max-reports",     "--max-reports",     U32(max_reports),        0,  5000 },
    { L"--min-free-mb",     "--min-free-mb",     U32(min_free_mb),        0, 1048576 },
    { L"--dump-timeout-ms", "--dump-timeout-ms", U32(dump_timeout_ms), 1000, 120000 },
};

static const wchar_t *split_eq(const wchar_t *arg, size_t *name_len) {
    const wchar_t *eq = wcschr(arg, L'=');
    *name_len = eq ? (size_t)(eq - arg) : wcslen(arg);
    return eq ? eq + 1 : NULL;
}

CliResult config_parse(Config *c, int argc, wchar_t **argv, char *err, size_t err_cch) {
    if (err && err_cch) err[0] = 0;
    for (int i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        if (!a) continue;
        size_t nlen; const wchar_t *inline_val = split_eq(a, &nlen);
        wchar_t name[40];
        if (nlen >= sizeof name / sizeof name[0]) nlen = sizeof name / sizeof name[0] - 1;
        wmemcpy(name, a, nlen); name[nlen] = 0;

        if (wft_wcs_ieq(name, L"--help") || wft_wcs_ieq(name, L"-h") || wft_wcs_ieq(name, L"-?") || wft_wcs_ieq(name, L"/?")) return CLI_HELP;
        if (wft_wcs_ieq(name, L"--version")) return CLI_VERSION;
        if (wft_wcs_ieq(name, L"--no-dump"))   { c->make_dump = false; continue; }
        if (wft_wcs_ieq(name, L"--redact"))    { c->redact = true; continue; }
        if (wft_wcs_ieq(name, L"--no-hotkey")) { c->hotkey = false; continue; }
        if (wft_wcs_ieq(name, L"--background")) continue;       /* accepted for compatibility (always true) */

        bool is_dir = wft_wcs_ieq(name, L"--report-dir");
        const Opt *opt = NULL;
        for (size_t k = 0; k < sizeof k_opts / sizeof k_opts[0]; ++k)
            if (wft_wcs_ieq(name, k_opts[k].name)) { opt = &k_opts[k]; break; }
        if (!opt && !is_dir) {
            char shown[64]; wft_wcs_to_utf8(a, shown, sizeof shown);
            wft_snprintf(err, err_cch, "unknown option '%s' (try --help)", shown);
            return CLI_ERROR;
        }
        const wchar_t *val = inline_val;
        if (!val) {
            if (i + 1 >= argc) { wft_snprintf(err, err_cch, "option '%s' requires a value", opt ? opt->label : "--report-dir"); return CLI_ERROR; }
            val = argv[++i];
        }
        if (is_dir) {
            if (!*val || wcslen(val) >= WFT_PATH_CCH - 64 || wcspbrk(val, L"<>\"|?*")) {
                wft_snprintf(err, err_cch, "option '--report-dir' needs a valid path (no < > \" | ? *, max %d chars)", WFT_PATH_CCH - 64);
                return CLI_ERROR;
            }
            wcsncpy(c->report_root, val, WFT_PATH_CCH - 1);
            c->report_root[WFT_PATH_CCH - 1] = 0;
            c->report_dir_set = true;
            continue;
        }
        uint32_t v;
        if (!wft_parse_u32(val, &v)) {
            wft_snprintf(err, err_cch, "option '%s' needs a non-negative whole number", opt->label);
            return CLI_ERROR;
        }
        uint32_t *dst = (uint32_t *)((char *)c + opt->off);
        *dst = v;
        clamp(c, opt->label, dst, opt->lo, opt->hi);
    }
    if (config_ring_capacity(c) < (int)(c->history_seconds * 1000u / c->interval_ms) && c->warning_count < 8)
        wft_snprintf(c->warnings[c->warning_count++], sizeof c->warnings[0],
                     "ring is capped at %d samples; effective history is about %u s", WFT_MAX_RING,
                     (unsigned)((uint64_t)config_ring_capacity(c) * c->interval_ms / 1000u));
    return CLI_OK;
}

int config_ring_capacity(const Config *c) {
    uint64_t n = (uint64_t)c->history_seconds * 1000u / (c->interval_ms ? c->interval_ms : 1000u);
    if (n < 4) n = 4;
    if (n > WFT_MAX_RING) n = WFT_MAX_RING;
    return (int)n;
}

const char *config_usage(void) {
    return
"WinFreezeTrace " WFT_VERSION " - freeze and hang recorder\n"
"\n"
"Usage: WinFreezeTrace.exe [options]\n"
"\n"
"Sampling and triggers:\n"
"  --interval <ms>          Sampling interval            (250..60000, default 1000)\n"
"  --history <s>            Seconds of RAM history       (30..600, default 180)\n"
"  --stall-ms <ms>          Watchdog lateness trigger    (500..30000, default 1500)\n"
"  --hang-timeout-ms <ms>   Foreground WM_NULL timeout   (50..5000, default 250)\n"
"  --hang-samples <n>       Failed probes in a row       (2..20, default 3)\n"
"  --cooldown <s>           Gap between auto captures    (5..86400, default 60)\n"
"  --no-hotkey              Do not register Ctrl+Alt+F12 (manual capture)\n"
"\n"
"Reports:\n"
"  --report-dir <path>      Report root (default %LOCALAPPDATA%\\WinFreezeTrace\\reports)\n"
"  --lead-window <s>        Seconds before the trigger treated as the lead-in (5..300, default 30)\n"
"  --max-reports <n>        Keep the newest n incident folders (0 = unlimited, default 50)\n"
"  --min-free-mb <n>        Skip minidumps below this much free space (default 1024, 0 = off)\n"
"  --no-dump                Never write target.dmp\n"
"  --dump-timeout-ms <ms>   Abort a minidump after this long (1000..120000, default 15000)\n"
"  --redact                 Mask user/computer names, serial numbers, window titles;\n"
"                           skip raw Event Log XML (does NOT sanitise target.dmp)\n"
"\n"
"Other:\n"
"  --help, --version\n"
"\n"
"Options also accept --name=value. Ctrl+Alt+F12 forces a capture.\n";
}
