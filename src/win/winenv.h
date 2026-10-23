/* winenv.h - the one place that pulls in the Windows headers. */
#ifndef WFT_WINENV_H
#define WFT_WINENV_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winevt.h>
#include <wct.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <winioctl.h>
#include <winver.h>
#include <sddl.h>
#include <powrprof.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wft_types.h"
#include "strutil.h"
#include "config.h"

#endif
