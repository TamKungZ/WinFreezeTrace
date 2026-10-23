/* trigger.h - pure decision logic for when to start a capture. */
#ifndef WFT_TRIGGER_H
#define WFT_TRIGGER_H
#include "wft_types.h"

typedef struct TriggerConfig {
    uint32_t stall_ms;          /* scheduler lateness threshold                */
    uint32_t hang_samples;      /* consecutive foreground timeouts             */
    uint32_t cooldown_s;        /* auto-capture cooldown                       */
    uint32_t resume_grace_ms;   /* ignore stalls this long after a resume      */
    uint32_t sleep_gap_ms;      /* lateness >= this is a sleep/hibernate gap   */
} TriggerConfig;

typedef struct TriggerState {
    uint32_t hung_streak;
    double   last_capture_ms;
    double   last_manual_ms;
    double   resume_until_ms;
} TriggerState;

typedef struct TriggerInput {
    double   now_ms;
    int      late_ms;
    int      probe_status;
    bool     manual;
} TriggerInput;

typedef struct TriggerDecision {
    TriggerReason reason;       /* TRIGGER_NONE when nothing should be captured */
    const char   *suppressed;   /* non-NULL: a trigger fired but was ignored    */
} TriggerDecision;

void trigger_config_default(TriggerConfig *c, uint32_t stall_ms, uint32_t hang_samples, uint32_t cooldown_s);
void trigger_init(TriggerState *s);
void trigger_note_resume(TriggerState *s, const TriggerConfig *c, double now_ms);
void trigger_note_capture(TriggerState *s, double now_ms);
TriggerDecision trigger_evaluate(TriggerState *s, const TriggerConfig *c, const TriggerInput *in);

#endif
