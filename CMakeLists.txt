cmake_minimum_required(VERSION 3.20)
project(WinFreezeTrace VERSION 0.4.0 LANGUAGES C)

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

# ---- The Windows application ----
if(WIN32)
  add_executable(WinFreezeTrace WIN32
    src/main.c
    src/win/platform.c
    src/win/metrics.c
    src/win/foreground.c
    src/win/ui.c
    src/win/procsnap.c
    src/win/threadinfo.c
    src/win/dump.c
    src/win/events.c
    src/win/sysinfo.c
    src/win/report.c
    src/version.rc)
  target_include_directories(WinFreezeTrace PRIVATE src src/core)
  target_compile_definitions(WinFreezeTrace PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601)
  target_link_libraries(WinFreezeTrace PRIVATE wft_core user32 psapi pdh wevtapi advapi32 dbghelp shell32 powrprof version)

  if(MSVC)
    target_compile_options(WinFreezeTrace PRIVATE /W4 /O2 /guard:cf /GS)
    target_link_options(WinFreezeTrace PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /DEPENDENTLOADFLAG:0x800)
  else()
    # MinGW-w64: wWinMain needs -municode.
    target_compile_options(WinFreezeTrace PRIVATE -Wall -Wextra -O2)
    target_link_options(WinFreezeTrace PRIVATE -municode -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)
  endif()
endif()
