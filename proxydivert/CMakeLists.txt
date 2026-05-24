cmake_minimum_required(VERSION 3.20)
project(ProxyDivert LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/divert_engine.cpp
    src/tcp_nat.cpp
    src/udp_nat.cpp
)

add_library(ProxyDivert STATIC ${SOURCES})

target_include_directories(ProxyDivert PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)

# WinDivert.lib — vendored binary
target_link_directories(ProxyDivert PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/windivert
)
target_link_libraries(ProxyDivert PUBLIC
    WinDivert.lib
    ws2_32.lib
)

target_compile_definitions(ProxyDivert PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
)

if(MSVC)
    target_compile_options(ProxyDivert PRIVATE /W3 /wd4100 /wd4244 /wd4267)
endif()
