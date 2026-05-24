cmake_minimum_required(VERSION 3.20)
project(ProxyRelay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse SOCKS5 client + ProxyConfig from proxyhook (no MinHook needed here —
# we only call socks5_client.cpp + config.cpp).
# hwid_gen.cpp подтягивается потому что config.cpp вызывает DeriveHwid.
set(PROXYHOOK_SHARED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../proxyhook/src/socks5_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../proxyhook/src/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../proxyhook/src/hwid_gen.cpp
)

set(SOURCES
    src/relay.cpp
    src/pid_proxy_map.cpp
    ${PROXYHOOK_SHARED_SOURCES}
)

add_library(ProxyRelay STATIC ${SOURCES})

target_include_directories(ProxyRelay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../proxyhook/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../orchestrator/deps
)

target_link_libraries(ProxyRelay PUBLIC
    ws2_32.lib
    bcrypt.lib
)

target_compile_definitions(ProxyRelay PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
)

if(MSVC)
    target_compile_options(ProxyRelay PRIVATE /W3 /wd4100 /wd4244 /wd4267)
endif()
