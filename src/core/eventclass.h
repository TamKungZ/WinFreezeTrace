/* eventclass.h - classify Windows Event Log records into diagnostic categories. */
#ifndef WFT_EVENTCLASS_H
#define WFT_EVENTCLASS_H
#include "wft_types.h"

/* Events within this many seconds of the trigger count as "near" it. */
#define WFT_NEAR_EVENT_S 180.0

typedef enum EventClass {
    EVC_NONE = 0,
    EVC_WHEA_UNCORRECTED,
    EVC_WHEA_CORRECTED,
    EVC_STORAGE,
    EVC_FILESYSTEM,
    EVC_DISPLAY,
    EVC_LIVEKERNEL,         /* WER LiveKernelEvent: GPU/driver watchdog timeouts */
    EVC_APPHANG,
    EVC_APPCRASH,
    EVC_THROTTLE,           /* Kernel-Processor-Power: CPU limited by firmware   */
    EVC_BUGCHECK,
    EVC_UNEXPECTED_SHUTDOWN,
    EVC_SLEEP_RESUME,
    EVC_POWER_OTHER,
    EVC_PERF,
    EVC_COUNT
} EventClass;

/* level: 1 critical, 2 error, 3 warning, 4 info. wer_livekernel: the WER record mentions LiveKernelEvent. */
EventClass  event_classify(const wchar_t *provider, uint32_t id, uint32_t level, bool wer_livekernel);
const char *event_class_name(EventClass c);

typedef struct EventCatStat {
    int    total;               /* in the whole query window                    */
    int    near_count;          /* within the near window of the trigger         */
    double nearest_age_s;       /* smallest |age|, WFT_NA when none              */
    char   example[112];        /* "Provider id" of the nearest event            */
} EventCatStat;

typedef struct EventSummary {
    EventCatStat cat[EVC_COUNT];
    int total_events;
} EventSummary;

void event_summary_init(EventSummary *s);
/* age_s > 0 means the event happened before the trigger. */
void event_summary_add(EventSummary *s, EventClass c, double age_s, double near_window_s, const char *example);

#endif
