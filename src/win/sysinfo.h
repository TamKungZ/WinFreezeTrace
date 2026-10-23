/* sysinfo.h - system, storage, driver, display and power snapshots for reports. */
#ifndef WFT_SYSINFO_H
#define WFT_SYSINFO_H
#include "platform.h"

typedef struct DriveFacts { double lowest_free_pct, lowest_free_gb; char lowest_drive[8]; } DriveFacts;
typedef struct PowerFacts { double max_temp_c, min_passive_pct; } PowerFacts;

void sysinfo_write_system(const WCHAR *dir, const Config *cfg);
void sysinfo_write_drives(const WCHAR *dir, DriveFacts *df);
void sysinfo_write_drivers(const WCHAR *dir, const Config *cfg);
void sysinfo_write_storage(const WCHAR *dir, const Config *cfg);
void sysinfo_write_displays(const WCHAR *dir, const Config *cfg);
void sysinfo_write_power(const WCHAR *dir, PowerFacts *pf);

#endif
