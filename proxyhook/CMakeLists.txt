cmake_minimum_required(VERSION 3.20)

# Bundle H — portable distribution: static-link MSVC runtime (/MT).
# Должно быть выставлено ДО project() чтобы CMP0091 NEW сработал.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CACHE STRING "MSVC runtime: /MT (static) для portable distribution")

project(ProxyHook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── MinHook (vendored) ──
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook")
set(MINHOOK_SOURCES
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)

set(PROXYHOOK_SOURCES
    src/dllmain.cpp
    src/config.cpp
    src/socks5_client.cpp
    src/hooks_tcp.cpp
    src/hooks_udp.cpp
    src/hooks_process.cpp
    src/hooks_hwid.cpp
    src/hwid_gen.cpp
    src/health.cpp
)

add_library(ProxyHook SHARED ${PROXYHOOK_SOURCES} ${MINHOOK_SOURCES})

target_include_directories(ProxyHook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINHOOK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../orchestrator/deps
)

target_link_libraries(ProxyHook PRIVATE
    ws2_32.lib
    advapi32.lib
    bcrypt.lib
    iphlpapi.lib
    ntdll.lib
)

target_compile_definitions(ProxyHook PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
)

# Keep exports minimal (DllMain only) — avoid accidental symbol leak
if(MSVC)
    target_compile_options(ProxyHook PRIVATE /W3 /wd4100 /wd4996)
endif()

set_target_properties(ProxyHook PROPERTIES
    OUTPUT_NAME "ProxyHook"
    PREFIX ""
)
