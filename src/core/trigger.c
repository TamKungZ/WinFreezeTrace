#include "trigger.h"

const char *trigger_name(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return "scheduler_stall";
        case TRIGGER_APP_HANG:        return "foreground_app_hang";
        case TRIGGER_MANUAL:          return "manual_hotkey";
        case TRIGGER_STORAGE_STALL:   return "storage_stall";
        default:                      return "unknown";
    }
}

const wchar_t *trigger_name_w(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return L"scheduler_stall";
        case TRIGGER_APP_HANG:        return L"foreground_app_hang";
        case TRIGGER_MANUAL:          return L"manual_hotkey";
        case TRIGGER_STORAGE_STALL:   return L"storage_stall";
        default:                      return L"unknown";
    }
}

int trigger_late_ms(double wall_ms, double awake_ms, uint32_t interval_ms, double *slept_ms) {
    double slept = 0.0;
    if (awake_ms >= 0.0 && wall_ms > awake_ms && wall_ms - awake_ms >= 1000.0) slept = wall_ms - awake_ms;
    if (slept_ms) *slept_ms = slept;
    double late = wall_ms - slept - (double)interval_ms;
    if (late < 0.0) late = 0.0;
    if (late > 2147483647.0) late = 2147483647.0;       /* a multi-week gap must not overflow the int */
    return (int)late;
}

void trigger_config_default(TriggerConfig *c, uint32_t stall_ms, uint32_t hang_samples, uint32_t cooldown_s) {
    c->stall_ms = stall_ms;
    c->hang_samples = hang_samples;
    c->cooldown_s = cooldown_s;
    c->resume_grace_ms = 15000;
    c->sleep_gap_ms = 60000;
    c->disk_latency_ms = 500.0;      /* same defaults as 0.2 */
    c->disk_queue = 10.0;
    c->disk_queue_samples = 2;
}

void trigger_init(TriggerState *s) {
    s->hung_streak = 0;
    s->last_capture_ms = -1e12;
    s->last_manual_ms = -1e12;
    s->resume_until_ms = -1e12;
    s->queue_streak = 0;
}

void trigger_note_resume(TriggerState *s, const TriggerConfig *c, double now_ms) {
    s->resume_until_ms = now_ms + (double)c->resume_grace_ms;
    s->hung_streak = 0;
    s->queue_streak = 0;
}

void trigger_note_capture(TriggerState *s, double now_ms) {
    s->last_capture_ms = now_ms;
    s->hung_streak = 0;
    s->queue_streak = 0;
}

TriggerDecision trigger_evaluate(TriggerState *s, const TriggerConfig *c, const TriggerInput *in) {
    TriggerDecision d = { TRIGGER_NONE, NULL };

    if (probe_is_hung(in->probe_status)) ++s->hung_streak; else s->hung_streak = 0;

    /* A person pressing the hotkey always wins, but is rate-limited to avoid key-repeat floods. */
    if (in->manual) {
        if (in->now_ms - s->last_manual_ms >= 3000.0) {
            s->last_manual_ms = in->now_ms;
            d.reason = TRIGGER_MANUAL;
            return d;
        }
        d.suppressed = "manual-rate-limit";
    }

    bool in_cooldown = (in->now_ms - s->last_capture_ms) < (double)c->cooldown_s * 1000.0;

    /* Storage: this is what a "nothing responds but nothing is at 100%" stall usually looks like. Latency is an average
     * over completed I/O, so one bad interval is already serious; a deep queue is also normal under a heavy copy, so it
     * has to hold for a few samples. Counters that are unavailable (WFT_NA < 0) never count. */
    bool slow_disk = c->disk_latency_ms > 0.0 && WFT_HAS(in->disk_latency_ms) && in->disk_latency_ms >= c->disk_latency_ms;
    bool deep_queue = c->disk_queue > 0.0 && WFT_HAS(in->disk_queue) && in->disk_queue >= c->disk_queue;
    if (deep_queue) ++s->queue_streak; else s->queue_streak = 0;
    if (slow_disk || (deep_queue && s->queue_streak >= c->disk_queue_samples)) {
        if (in->now_ms < s->resume_until_ms)  d.suppressed = "resume-grace";      /* disks spin up after a resume */
        else if (in_cooldown)                 d.suppressed = "cooldown";
        else { d.reason = TRIGGER_STORAGE_STALL; d.suppressed = NULL; return d; }
    }

    if (in->late_ms >= (int)c->stall_ms) {
        if (in->late_ms >= (int)c->sleep_gap_ms)          d.suppressed = "sleep-or-hibernate-gap";
        else if (in->now_ms < s->resume_until_ms)         d.suppressed = "resume-grace";
        else if (in_cooldown)                             d.suppressed = "cooldown";
        else { d.reason = TRIGGER_SCHEDULER_STALL; d.suppressed = NULL; return d; }
    }
    if (s->hung_streak >= c->hang_samples) {
        if (in_cooldown) { if (!d.suppressed) d.suppressed = "cooldown"; }
        else { d.reason = TRIGGER_APP_HANG; d.suppressed = NULL; return d; }
    }
    return d;
}
