/* config.h - command-line configuration with strict validation. */
#ifndef WFT_CONFIG_H
#define WFT_CONFIG_H

#include "wft_types.h"

typedef struct Config {
    uint32_t interval_ms;        /* sampling period                         */
    uint32_t history_seconds;    /* RAM history requested                   */
    uint32_t stall_ms;           /* watchdog lateness that triggers         */
    uint32_t hang_timeout_ms;    /* WM_NULL probe timeout                   */
    uint32_t hang_samples;       /* consecutive failed probes               */
    uint32_t cooldown_seconds;   /* min gap between automatic captures      */
    uint32_t lead_seconds;       /* "lead-in" window used by the analysis   */
    uint32_t max_reports;        /* retention: keep newest N (0 = no limit) */
    uint32_t min_free_mb;        /* skip dump / heavy capture below this    */
    uint32_t dump_timeout_ms;    /* abort a minidump that takes longer      */
    bool     make_dump;
    bool     redact;             /* mask user/computer names, serials, titles */
    bool     hotkey;
    bool     report_dir_set;
    wchar_t  report_root[WFT_PATH_CCH];
    int      warning_count;
    char     warnings[8][160];   /* clamped values etc, echoed into reports */
} Config;

typedef enum CliResult { CLI_OK = 0, CLI_ERROR, CLI_HELP, CLI_VERSION } CliResult;

void        config_defaults(Config *c);
/* argv[0] is skipped. On CLI_ERROR a message is written to err. */
CliResult   config_parse(Config *c, int argc, wchar_t **argv, char *err, size_t err_cch);
/* Effective number of ring samples after applying MAX ring and interval. */
int         config_ring_capacity(const Config *c);
const char *config_usage(void);

#endif
