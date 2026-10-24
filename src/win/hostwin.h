/* hostwin.h - hidden top-level window on its own thread: the Ctrl+Alt+F12 hotkey, power broadcasts (suspend/resume),
 *             session end and the --exit message from another copy. It has no visible UI: the tray icon, balloons
 *             and settings live in WinFreezeTrace Manager, which talks to the recorder through control.h. */
#ifndef WFT_HOSTWIN_H
#define WFT_HOSTWIN_H
#include "platform.h"

typedef struct HostOptions {
    bool hotkey;                /* register Ctrl+Alt+F12 */
} HostOptions;

/* stop_event is signalled on logoff/shutdown and when another copy runs --exit.
 * Returns false if the thread could not start or the window could not be created. */
bool host_start(HINSTANCE inst, HANDLE stop_event, const HostOptions *opt);
void host_stop(void);
bool host_take_resume(void);     /* true once per suspend/resume notification */

/* --exit: ask the recorder running in this session to quit. */
void host_request_running_instance_exit(void);

#endif
