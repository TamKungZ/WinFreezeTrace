/* findings.h - heuristic analysis of a capture: ranked findings with evidence. */
#ifndef WFT_FINDINGS_H
#define WFT_FINDINGS_H

#include <stdio.h>
#include "wft_types.h"
#include "stats.h"
#include "eventclass.h"

typedef enum Confidence { CONF_INFO = 0, CONF_LOW, CONF_MED, CONF_HIGH } Confidence;

#define WFT_MAX_FINDINGS 40

typedef struct Finding {
    Confidence conf;
    char id[32];
    char title[200];
    char evidence[420];
    char next[300];
} Finding;

typedef struct AnalysisInput {
    TriggerReason reason;
    const Sample *s;                /* chronological; s[n-1] is the trigger sample */
    int           n;
    uint32_t      stall_ms, logical_cpus, lead_seconds;
    bool          wait_cycle;
    char          ui_blocker[200];  /* description of what the UI thread waited on, or "" */
    bool          ui_cross_process;
    bool          resumed_recently; /* a sleep/resume notification was seen recently */
    EventSummary  ev;
    double        lowest_free_pct, lowest_free_gb;   /* fixed drives; WFT_NA if unknown */
    char          lowest_free_drive[8];
    double        max_temp_c, min_passive_limit_pct; /* WFT_NA if unknown */
} AnalysisInput;

typedef struct AnalysisResult {
    Finding    f[WFT_MAX_FINDINGS];
    int        count;
    int        lead_start;          /* first sample index inside the lead window */
    bool       baseline_overlaps;   /* ring too short to separate lead from baseline */
    WindowStat lead[MI_COUNT], base[MI_COUNT], all[MI_COUNT];
} AnalysisResult;

void        analysis_run(const AnalysisInput *in, AnalysisResult *out);
const char *confidence_name(Confidence c);

void analysis_write_findings_text(FILE *f, const AnalysisResult *r);
void analysis_write_metric_table(FILE *f, const AnalysisResult *r, uint32_t lead_seconds);
void analysis_write_event_summary(FILE *f, const EventSummary *e);
void analysis_write_findings_json(FILE *f, const AnalysisResult *r);
void analysis_write_metrics_json(FILE *f, const AnalysisResult *r);

#endif
