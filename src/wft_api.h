#ifndef WFT_API_H
#define WFT_API_H
#include <windows.h>

#ifdef WFT_BUILDING_DLL
#  define WFT_API __declspec(dllexport)
#else
#  define WFT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

WFT_API int WftRun(HINSTANCE inst);
WFT_API const char *WftVersion(void);

#ifdef __cplusplus
}
#endif
#endif
