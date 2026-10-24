/* platform.h - process hardening, time, files, private directories, logging. */
#ifndef WFT_PLATFORM_H
#define WFT_PLATFORM_H
#include "winenv.h"

void   plat_init(void);                       /* QPC frequency */
void   plat_harden_process(void);             /* DLL search path, heap corruption, error mode */
double plat_qpc_ms(void);
/* Milliseconds on a clock that does NOT advance while the machine sleeps/hibernates; < 0 if unsupported.
 * Comparing it with plat_qpc_ms() reveals sleep gaps even when no power notification is delivered. */
double plat_unbiased_ms(void);
void   plat_local_time(WftTime *t);
DWORD  plat_self_pid(void);

typedef enum DirResult { DIR_CREATED, DIR_EXISTS, DIR_FAILED } DirResult;
/* Security attributes granting access to the current user and SYSTEM only (for the control pipe). false = unavailable. */
bool   plat_private_security(SECURITY_ATTRIBUTES *sa);
/* Create a directory readable only by the current user and SYSTEM (protected DACL). */
DirResult plat_create_private_dir(const WCHAR *path);
/* Like plat_create_private_dir, but also creates missing parent folders (only the last one is made private). */
DirResult plat_create_private_dir_tree(const WCHAR *path);
/* Creates "<local appdata>\WinFreezeTrace" (+ \reports) and returns both paths. */
void   plat_default_paths(WCHAR *app_root, size_t app_cch, WCHAR *reports, size_t rep_cch);
bool   plat_exe_path(WCHAR *out, size_t cch);   /* full path of the running .exe */
bool   plat_abs_path(const WCHAR *in, WCHAR *out, size_t cch);
bool   plat_free_mb(const WCHAR *path, uint64_t *mb);
FILE  *plat_fopen_w(const WCHAR *path);       /* binary write, NULL on failure */
bool   plat_pid_exe(DWORD pid, WCHAR *out, size_t cch);   /* full image path, limited rights */

/* Console output for --help/--version/errors when launched from a terminal. */
void   plat_print(const char *text);

/* Tiny size-capped log in the app folder. Never blocks capture on failure. */
void   log_init(const WCHAR *dir);
void   log_msg(const char *level, const char *fmt, ...);
/* fmt contains exactly one %s that receives the wide string (converted to UTF-8; never use %ls with log_msg). */
void   log_msg_w(const char *level, const char *fmt, const WCHAR *w);

#endif
