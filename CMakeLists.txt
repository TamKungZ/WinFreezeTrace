cmake_minimum_required(VERSION 3.20)
project(WinFreezeTrace VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(WinFreezeTrace WIN32 src/main.c src/version.rc)
target_compile_definitions(WinFreezeTrace PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(WinFreezeTrace PRIVATE user32 psapi pdh wevtapi advapi32 dbghelp shell32)

if(MSVC)
  target_compile_options(WinFreezeTrace PRIVATE /W4 /O2 /guard:cf)
  target_link_options(WinFreezeTrace PRIVATE /DYNAMICBASE /NXCOMPAT)
else()
  target_compile_options(WinFreezeTrace PRIVATE -Wall -Wextra -O2)
endif()
