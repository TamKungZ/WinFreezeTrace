/* report.h - orchestrates one incident capture and writes the summary. */
#ifndef WFT_REPORT_H
#define WFT_REPORT_H
#include "platform.h"
#include "foreground.h"

/* samples: chronological, last element is the trigger sample. Returns false if no folder could be created. */
bool capture_run(const Config *cfg, TriggerReason reason, const Sample *samples, int n, const ForegroundInfo *fg,
                 bool resumed_recently, WCHAR *out_dir, size_t out_cch);

/* Delete the oldest incident folders beyond `keep` (0 = keep everything). */
void report_prune(const WCHAR *root, uint32_t keep);

#endif
