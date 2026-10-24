cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
project(WinFreezeTrace VERSION 0.7.0 LANGUAGES C)

# One version everywhere: fail the configure step instead of shipping mismatched numbers.
file(STRINGS src/core/wft_types.h _wft_hdr_ver REGEX "^#define WFT_VERSION")
if(NOT _wft_hdr_ver MATCHES "\"${PROJECT_VERSION}\"")
  message(FATAL_ERROR "WFT_VERSION in src/core/wft_types.h (${_wft_hdr_ver}) does not match project VERSION ${PROJECT_VERSION}")
endif()
string(REPLACE "." "," _wft_rc_ver "${PROJECT_VERSION}")
file(STRINGS src/version.rc _wft_rc_lines REGEX "FILEVERSION|ProductVersion|FileVersion")
foreach(_l IN LISTS _wft_rc_lines)
  if(NOT (_l MATCHES "${PROJECT_VERSION}" OR _l MATCHES "${_wft_rc_ver},0"))
    message(FATAL_ERROR "src/version.rc is out of date (${PROJECT_VERSION} expected): ${_l}")
  endif()
endforeach()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---- Platform-independent core (no Windows headers); builds and tests anywhere ----
add_library(wft_core STATIC
  src/core/strutil.c
  src/core/config.c
  src/core/ring.c
  src/core/stats.c
  src/core/trigger.c
  src/core/eventclass.c
  src/core/findings.c
  src/core/retention.c)
target_include_directories(wft_core PUBLIC src/core)
if(MSVC)
  target_compile_definitions(wft_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
  target_compile_options(wft_core PRIVATE -Wall -Wextra)
endif()

option(WFT_BUILD_TESTS "Build the unit tests" ON)
if(WFT_BUILD_TESTS)
  enable_testing()
  add_executable(wft_tests tests/test_core.c)
  target_link_libraries(wft_tests PRIVATE wft_core)
  add_test(NAME core COMMAND wft_tests)
endif()

# ---- The Windows application: thin exe + core DLL ----
if(WIN32)
  add_library(WinFreezeTraceCore SHARED
    src/engine.c
    src/win/platform.c
    src/win/metrics.c
    src/win/foreground.c
    src/win/hostwin.c
    src/win/control.c
    src/win/procsnap.c
    src/win/threadinfo.c
    src/win/dump.c
    src/win/events.c
    src/win/sysinfo.c
    src/win/report.c
    src/version.rc)
  set_target_properties(WinFreezeTraceCore PROPERTIES PREFIX "")
  target_include_directories(WinFreezeTraceCore PRIVATE src src/core)
  target_compile_definitions(WinFreezeTraceCore PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601 WFT_BUILDING_DLL WFT_RC_DLL)
  target_link_libraries(WinFreezeTraceCore PRIVATE wft_core user32 psapi pdh wevtapi advapi32 dbghelp shell32 powrprof version gdi32)

  add_executable(WinFreezeTrace WIN32 src/launcher.c src/version.rc)
  target_compile_definitions(WinFreezeTrace PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601)
  target_link_libraries(WinFreezeTrace PRIVATE user32)

  if(MSVC)
    foreach(t WinFreezeTraceCore WinFreezeTrace)
      target_compile_options(${t} PRIVATE /W4 /O2 /guard:cf /GS)
      target_link_options(${t} PRIVATE /DYNAMICBASE /NXCOMPAT /DEPENDENTLOADFLAG:0x800)
    endforeach()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      target_link_options(WinFreezeTraceCore PRIVATE /HIGHENTROPYVA)
      target_link_options(WinFreezeTrace PRIVATE /HIGHENTROPYVA)
    endif()
    target_compile_definitions(WinFreezeTrace PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    foreach(t WinFreezeTraceCore WinFreezeTrace)
      target_compile_options(${t} PRIVATE -Wall -Wextra -O2)
      target_link_options(${t} PRIVATE -static -static-libgcc -Wl,--dynamicbase -Wl,--nxcompat)
    endforeach()
    target_link_options(WinFreezeTrace PRIVATE -municode)
  endif()
endif()
