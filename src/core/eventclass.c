#include "eventclass.h"
#include "strutil.h"

#include <string.h>

static bool starts(const wchar_t *p, const wchar_t *x) { return wft_wcs_istarts(p, x); }
static bool eq(const wchar_t *p, const wchar_t *x)     { return wft_wcs_ieq(p, x); }
static bool has(const wchar_t *p, const wchar_t *x)    { return wft_wcs_icontains(p, x); }

EventClass event_classify(const wchar_t *p, uint32_t id, uint32_t level, bool wer_live) {
    if (!p) return EVC_NONE;
    if (has(p, L"WHEA")) return (level >= 1 && level <= 2) ? EVC_WHEA_UNCORRECTED : EVC_WHEA_CORRECTED;   /* level 0 = unknown, not an error */

    if (eq(p, L"disk") || eq(p, L"storahci") || eq(p, L"stornvme") || eq(p, L"storport") ||
        eq(p, L"storufs") || eq(p, L"partmgr") || starts(p, L"iaStor") || starts(p, L"nvme") ||
        has(p, L"StorPort") || has(p, L"Storage-ClassPnP"))
        return EVC_STORAGE;

    if (eq(p, L"Ntfs") || has(p, L"-Ntfs") || eq(p, L"volmgr") || has(p, L"ReFS") ||
        eq(p, L"exfat") || eq(p, L"fastfat") || eq(p, L"volsnap"))
        return EVC_FILESYSTEM;

    if (eq(p, L"Display") || starts(p, L"nvlddmkm") || starts(p, L"amdkmdag") || starts(p, L"amdwddmg") ||
        starts(p, L"igfx") || has(p, L"dxgkrnl"))
        return EVC_DISPLAY;

    if (eq(p, L"Windows Error Reporting")) return wer_live ? EVC_LIVEKERNEL : EVC_NONE;
    if (eq(p, L"Application Hang"))        return EVC_APPHANG;
    if (eq(p, L"Application Error"))       return EVC_APPCRASH;

    if (has(p, L"Kernel-Processor-Power")) return id == 37 ? EVC_THROTTLE : EVC_POWER_OTHER;
    if (has(p, L"Kernel-Power")) {
        if (id == 41) return EVC_UNEXPECTED_SHUTDOWN;
        if (id == 42 || id == 107 || id == 506 || id == 507) return EVC_SLEEP_RESUME;
        return EVC_POWER_OTHER;
    }
    if (has(p, L"Power-Troubleshooter")) return id == 1 ? EVC_SLEEP_RESUME : EVC_POWER_OTHER;
    if (eq(p, L"EventLog"))              return id == 6008 ? EVC_UNEXPECTED_SHUTDOWN : EVC_NONE;
    if (has(p, L"WER-SystemErrorReporting")) return EVC_BUGCHECK;
    if (has(p, L"Diagnostics-Performance"))  return EVC_PERF;
    return EVC_NONE;
}

const char *event_class_name(EventClass c) {
    static const char *const n[EVC_COUNT] = {
        "other", "whea_uncorrected", "whea_corrected", "storage", "filesystem", "display_driver",
        "livekernel_event", "application_hang", "application_crash", "cpu_throttle_firmware",
        "bugcheck", "unexpected_shutdown", "sleep_resume", "power_other", "performance_diag"
    };
    return (c >= 0 && c < EVC_COUNT) ? n[c] : "other";
}

void event_summary_init(EventSummary *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < EVC_COUNT; ++i) s->cat[i].nearest_age_s = WFT_NA;
}

void event_summary_add(EventSummary *s, EventClass c, double age_s, double near_window_s, const char *example) {
    if (c <= EVC_NONE || c >= EVC_COUNT) return;
    EventCatStat *k = &s->cat[c];
    double mag = age_s < 0 ? -age_s : age_s;
    ++k->total;
    ++s->total_events;
    bool near = age_s <= near_window_s && age_s >= -near_window_s;
    if (near) ++k->near_count;
    if (!WFT_HAS(k->nearest_age_s) || mag < k->nearest_age_s) {
        k->nearest_age_s = mag;
        if (example) wft_snprintf(k->example, sizeof k->example, "%s", example);
    }
}
