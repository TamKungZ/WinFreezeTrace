/* startup.h - "Start with Windows": the per-user entry that Settings > Apps > Startup and Task Manager list. */
#ifndef WFT_STARTUP_H
#define WFT_STARTUP_H
#include "platform.h"

/* True when the Run entry exists and has not been switched off in Task Manager / Settings. */
bool startup_is_enabled(void);
/* enable: write "<this exe>" + args (may be NULL/empty) and clear any "disabled" flag.
 * disable: remove the entry. Returns false if the registry could not be updated. */
bool startup_set(bool enable, const WCHAR *args);

#endif
