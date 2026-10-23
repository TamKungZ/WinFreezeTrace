#include "trigger.h"

const char *trigger_name(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return "scheduler_stall";
        case TRIGGER_APP_HANG:        return "foreground_app_hang";
        case TRIGGER_MANUAL:          return "manual_hotkey";
        default:                      return "unknown";
    }
}

const wchar_t *trigger_name_w(TriggerReason r) {
    switch (r) {
        case TRIGGER_SCHEDULER_STALL: return L"scheduler_stall";
        case TRIGGER_APP_HANG:        return L"foreground_app_hang";
        case TRIGGER_MANUAL:          return L"manual_hotkey";
        default:                      return L"unknown";
    }
}

void trigger_config_default(TriggerConfig *c, uint32_t stall_ms, uint32_t hang_samples, uint32_t cooldown_s) {
    c->stall_ms = stall_ms;
    c->hang_samples = hang_samples;
    c->cooldown_s = cooldown_s;
    c->resume_grace_ms = 15000;
    c->sleep_gap_ms = 60000;
}

void trigger_init(TriggerState *s) {
    s->hung_streak = 0;
    s->last_capture_ms = -1e12;
    s->last_manual_ms = -1e12;
    s->resume_until_ms = -1e12;
}

void trigger_note_resume(TriggerState *s, const TriggerConfig *c, double now_ms) {
    s->resume_until_ms = now_ms + (double)c->resume_grace_ms;
    s->hung_streak = 0;
}

void trigger_note_capture(TriggerState *s, double now_ms) {
    s->last_capture_ms = now_ms;
    s->hung_streak = 0;
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
