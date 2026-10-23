/* ui.h - hidden top-level window: hotkey, power broadcasts, session end. */
#ifndef WFT_UI_H
#define WFT_UI_H
#include "platform.h"

/* stop_event is signalled on logoff/shutdown. Returns false if the thread could not start. */
bool ui_start(HINSTANCE inst, HANDLE stop_event, bool want_hotkey);
void ui_stop(void);
bool ui_hotkey_registered(void);
bool ui_take_manual(void);     /* true once per hotkey press */
bool ui_take_resume(void);     /* true once per suspend/resume notification */

#endif
