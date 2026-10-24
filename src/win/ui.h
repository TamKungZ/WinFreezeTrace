/* ui.h - hidden top-level window on its own thread: hotkey, power broadcasts, session end,
 *        and the notification-area (tray) icon with its on/off, start-up and exit controls. */
#ifndef WFT_UI_H
#define WFT_UI_H
#include "platform.h"

typedef struct UiOptions {
    bool         hotkey;        /* register Ctrl+Alt+F12                                         */
    bool         tray;          /* show the notification-area icon                               */
    bool         notify;        /* allow balloon notifications                                   */
    const WCHAR *reports_dir;   /* opened by "Open reports folder"                               */
    const WCHAR *run_args;      /* command-line options stored by "Start with Windows" (or NULL) */
} UiOptions;

/* stop_event is signalled on logoff/shutdown and by the tray "Exit" command.
 * wake_event (auto-reset) is signalled whenever the sampling loop should act now (hotkey, on/off toggle).
 * Returns false if the thread could not start or the window could not be created. */
bool ui_start(HINSTANCE inst, HANDLE stop_event, HANDLE wake_event, const UiOptions *opt);
void ui_stop(void);
bool ui_hotkey_registered(void);
bool ui_take_manual(void);     /* true once per hotkey press */
bool ui_take_resume(void);     /* true once per suspend/resume notification */
bool ui_paused(void);          /* monitoring switched off from the tray */

/* Called from the sampling thread; both are cheap and non-blocking. */
void ui_set_capturing(bool on);                     /* orange icon while a report is being written */
void ui_capture_done(const WCHAR *dir, bool ok);    /* balloon + "click to open the report folder" */

/* Second-instance helpers: talk to the instance already running in this session. */
void ui_ping_running_instance(void);                /* it shows a "already running" balloon */
void ui_request_running_instance_exit(void);        /* it quits (same as tray > Exit)       */

#endif
