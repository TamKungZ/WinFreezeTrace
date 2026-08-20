cmake_minimum_required(VERSION 3.20)
project(WinFreezeTrace VERSION 0.2.1 LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "WinFreezeTrace targets Windows only.")
endif()

add_executable(WinFreezeTrace
    src/main.c
)

target_compile_features(WinFreezeTrace PRIVATE c_std_11)
target_compile_definitions(WinFreezeTrace PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(WinFreezeTrace PRIVATE /W4 /permissive-)
else()
    target_compile_options(WinFreezeTrace PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(WinFreezeTrace PRIVATE
    pdh
    wevtapi
    advapi32
    user32
)

install(TARGETS WinFreezeTrace RUNTIME DESTINATION .)
