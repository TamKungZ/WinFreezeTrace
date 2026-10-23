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

void events_capture(const WCHAR *dir, const Config *cfg, EventsResult *res);

#endif
