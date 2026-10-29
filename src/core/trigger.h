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
    double   disk_latency_ms;   /* worst-disk latency that triggers; 0 = off   */
    double   disk_queue;        /* worst-disk queue that triggers; 0 = off     */
    uint32_t disk_queue_samples;/* consecutive samples the queue must hold     */
} TriggerConfig;

typedef struct TriggerState {
    uint32_t hung_streak;
    double   last_capture_ms;
    double   last_manual_ms;
    double   resume_until_ms;
    uint32_t queue_streak;
} TriggerState;

typedef struct TriggerInput {
    double   now_ms;
    int      late_ms;
    int      probe_status;
    bool     manual;
    double   disk_latency_ms;   /* worst single physical disk, WFT_NA if unknown */
    double   disk_queue;        /* worst single physical disk (instantaneous)    */
} TriggerInput;

typedef struct TriggerDecision {
    TriggerReason reason;       /* TRIGGER_NONE when nothing should be captured */
    const char   *suppressed;   /* non-NULL: a trigger fired but was ignored    */
} TriggerDecision;

/* Scheduler lateness of one watchdog wake-up, excluding time the machine spent asleep.
 *   wall_ms   elapsed time on a clock that keeps running during sleep (QPC)
 *   awake_ms  elapsed time on a clock that stops during sleep (unbiased interrupt time); < 0 = unknown
 * *slept_ms receives the detected sleep duration (0 when none, or when it is under one second). */
int  trigger_late_ms(double wall_ms, double awake_ms, uint32_t interval_ms, double *slept_ms);

void trigger_config_default(TriggerConfig *c, uint32_t stall_ms, uint32_t hang_samples, uint32_t cooldown_s);
void trigger_init(TriggerState *s);
void trigger_note_resume(TriggerState *s, const TriggerConfig *c, double now_ms);
void trigger_note_capture(TriggerState *s, double now_ms);
TriggerDecision trigger_evaluate(TriggerState *s, const TriggerConfig *c, const TriggerInput *in);

#endif
