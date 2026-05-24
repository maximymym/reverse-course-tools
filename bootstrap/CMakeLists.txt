cmake_minimum_required(VERSION 3.20)
project(DotaFarmBootstrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DotaFarmBootstrap WIN32 main.cpp)

target_compile_definitions(DotaFarmBootstrap PRIVATE
    WIN32_LEAN_AND_MEAN UNICODE _UNICODE
)

if(MSVC)
    target_compile_options(DotaFarmBootstrap PRIVATE /W4 /utf-8 /O2 /GS /sdl)
    target_link_options(DotaFarmBootstrap PRIVATE
        /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()

target_link_libraries(DotaFarmBootstrap PRIVATE winhttp comctl32 ole32)
