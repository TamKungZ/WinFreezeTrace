/* control.h - the recorder's control surface: shared monitoring state plus a per-user named pipe that a separate
 *             program (WinFreezeTrace Manager) uses to show the state and to turn monitoring on/off, request a
 *             capture or ask the recorder to quit. No UI lives in the recorder.
 *
 * Pipe:      \\.\pipe\WinFreezeTrace.Control.v1.<session id>   (access: current user and SYSTEM, local clients only)
 * Protocol:  the client connects, writes one ASCII line, reads one line of JSON, disconnects.
 *              STATUS | PAUSE | RESUME | CAPTURE | EXIT
 *            Every reply is {"ok":true|false,...,"proto":1,...}; STATUS fields are listed in control.c.
 */
#ifndef WFT_CONTROL_H
#define WFT_CONTROL_H
#include "platform.h"

#define WFT_CONTROL_PROTO 1

typedef struct ControlOptions {
    const WCHAR *reports_dir;      /* reported to the client (it opens the folder)   */
} ControlOptions;

/* stop_event: signalled by the EXIT command. wake_event (auto-reset): signalled when the sampling loop must act now.
 * Returns false when the pipe could not be created (the recorder still works, it just cannot be controlled). */
bool control_start(HANDLE stop_event, HANDLE wake_event, const ControlOptions *opt);
void control_stop(void);

/* Monitoring switch (sampling thread polls it; the pipe and the hotkey window change it). */
bool control_paused(void);
void control_set_paused(bool paused);

/* Manual capture request from the hotkey or the CAPTURE command: true once per request. */
void control_request_capture(void);
bool control_take_capture(void);

/* Sampling thread -> clients. */
void control_set_capturing(bool on);
void control_capture_done(const WCHAR *dir, bool ok);
void control_set_hotkey_registered(bool ok);

#endif
