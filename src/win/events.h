/* events.h - Event Log export (raw XML) and structured, classified digest. */
#ifndef WFT_EVENTS_H
#define WFT_EVENTS_H
#include "platform.h"
#include "eventclass.h"

typedef struct EventsResult {
    EventSummary summary;
    int digest_rows;
    int channels_ok;
} EventsResult;

/* trigger_ft: FILETIME (UTC) of the moment the trigger fired; event ages are measured from it, not from when this runs. */
void events_capture(const WCHAR *dir, const Config *cfg, ULONGLONG trigger_ft, EventsResult *res);

#endif
