cmake_minimum_required(VERSION 3.20)

# Bundle H — portable distribution: static-link MSVC runtime ВО ВСЕ цели
# (DotaFarm.exe + ProxyDivert + ProxyRelay + ProxyHook). Должно быть выставлено
# ДО project() чтобы CMP0091 NEW подхватил глобально и наследовался в add_subdirectory.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CACHE STRING "MSVC runtime: /MT (static) для portable distribution")

project(DotaFarm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sub-libraries ──
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proxydivert proxydivert)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proxyrelay  proxyrelay)

# ── Sources ──
set(SOURCES
    src/main.cpp
    src/orchestrator.cpp
    src/steam_launcher.cpp
    src/dota_launcher.cpp
    src/dota_minifier.cpp
    src/bot_dota_dir.cpp
    src/injector.cpp
    src/config.cpp
    src/gui.cpp
    src/theme.cpp
    src/gsi_server.cpp
    src/gsi_install.cpp
    src/hwid_spoof.cpp
    src/scripts_sync.cpp
    src/proxy_service.cpp
    src/singbox_supervisor.cpp
    src/hero_portraits.cpp
    src/crash_watchdog.cpp
    src/window_tiler.cpp
    src/orchestrator_ipc.cpp
    src/orchestrator_ipc_slave.cpp
    src/orchestrator_relay_client.cpp
    src/match_pairing_fsm.cpp
    src/role_rotation.cpp
    src/strategy_writer.cpp
    src/mem_reclaim.cpp
)

# ── ImGui (vendored from starter project) ──
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../starter/deps/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${IMGUI_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/stb
    ${IMGUI_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ProxyDivert
    ProxyRelay
    d3d11.lib
    dxgi.lib
    dwmapi.lib
    advapi32.lib
    shell32.lib
    winhttp.lib
    bcrypt.lib
    ws2_32.lib
    iphlpapi.lib
    psapi.lib
)

# Require admin (needed for symlinks in BotSteam setup + DLL injection +
# WinDivert driver load)
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()

# Suppress MSVC warnings for third-party code
target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── Embedded resources (Stream B / B2) ─────────────────────────────────
# Four artifacts encrypted (AES-256-GCM, HKDF from src/crypto/embed_secret.h
# with per-artifact (salt, info) pair) and embedded as RT_RCDATA:
#   ANDROMEDA_DLL  (DFDL) — Andromeda-Dota2-Base.dll  (forked VS project, off-repo)
#   PROXYHOOK_DLL  (DFDL) — ProxyHook.dll              (built in-tree)
#   SPOOFER_SYS    (DFSP) — kernel SMBIOS spoofer driver
#   KDU_EXE        (DFSP) — Kernel Driver Utility (CVE-2018-19320 exploit)
# payload_loader::LoadEmbeddedResource decrypts at runtime; DLLs are
# manual-mapped without disk hit, sys/exe are staged to TEMP only when
# the HWID spoof feature is actually invoked.
set(EMBEDDED_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
file(MAKE_DIRECTORY "${EMBEDDED_DIR}")

set(ANDROMEDA_SRC_DLL "C:/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll")
set(PROXYHOOK_SRC_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../proxyhook/build/Release/ProxyHook.dll")
set(SPOOFER_SRC_SYS   "C:/temp/spoofer.sys")
set(KDU_SRC_EXE       "C:/temp/kdu.exe")
set(ENCRYPT_SCRIPT    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/encrypt_resource.py")
set(EMBED_SECRET_HDR  "${CMAKE_CURRENT_SOURCE_DIR}/src/crypto/embed_secret.h")
set(ANDROMEDA_ENC     "${EMBEDDED_DIR}/andromeda_encrypted.bin")
set(PROXYHOOK_ENC     "${EMBEDDED_DIR}/proxyhook_encrypted.bin")
set(SPOOFER_ENC       "${EMBEDDED_DIR}/spoofer_encrypted.bin")
set(KDU_ENC           "${EMBEDDED_DIR}/kdu_encrypted.bin")

# RC looks for "embedded/*.bin" relative to the .rc file's location. Inject
# /I<build-dir> so the resource compiler can find generated blobs.
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_BINARY_DIR}\"")

find_program(PYTHON3 NAMES python3 python)

# Always create placeholders so RC has files to embed even if a given source
# is missing. Runtime decrypt of an empty resource fails gracefully (HWID
# spoof in particular skips itself if SPOOFER_SYS / KDU_EXE blob is empty).
foreach(_blob IN ITEMS "${ANDROMEDA_ENC}" "${PROXYHOOK_ENC}" "${SPOOFER_ENC}" "${KDU_ENC}")
    if(NOT EXISTS "${_blob}")
        file(WRITE "${_blob}" "")
    endif()
endforeach()

if(PYTHON3 AND EXISTS "${ANDROMEDA_SRC_DLL}")
    add_custom_command(
        OUTPUT  "${ANDROMEDA_ENC}"
        COMMAND ${PYTHON3} "${ENCRYPT_SCRIPT}"
                --in            "${ANDROMEDA_SRC_DLL}"
                --out           "${ANDROMEDA_ENC}"
                --secret-header "${EMBED_SECRET_HDR}"
                --magic         DFDL
                --salt          dotafarm-dll-v1
                --info          dll-embed
        DEPENDS "${ANDROMEDA_SRC_DLL}" "${ENCRYPT_SCRIPT}" "${EMBED_SECRET_HDR}"
        COMMENT "Encrypting Andromeda-Dota2-Base.dll for embedding"
        VERBATIM
    )
else()
    message(WARNING
        "ANDROMEDA_SRC_DLL not found at '${ANDROMEDA_SRC_DLL}' OR python missing — "
        "ANDROMEDA_DLL resource will be empty. Build VS project in "
        "C:\\temp\\andromeda_src first.")
endif()

if(PYTHON3 AND EXISTS "${PROXYHOOK_SRC_DLL}")
    add_custom_command(
        OUTPUT  "${PROXYHOOK_ENC}"
        COMMAND ${PYTHON3} "${ENCRYPT_SCRIPT}"
                --in            "${PROXYHOOK_SRC_DLL}"
                --out           "${PROXYHOOK_ENC}"
                --secret-header "${EMBED_SECRET_HDR}"
                --magic         DFDL
                --salt          dotafarm-dll-v1
                --info          dll-embed
        DEPENDS "${PROXYHOOK_SRC_DLL}" "${ENCRYPT_SCRIPT}" "${EMBED_SECRET_HDR}"
        COMMENT "Encrypting ProxyHook.dll for embedding"
        VERBATIM
    )
else()
    message(WARNING
        "PROXYHOOK_SRC_DLL not found at '${PROXYHOOK_SRC_DLL}' OR python missing — "
        "PROXYHOOK_DLL resource will be empty. Build ProxyHook target first.")
endif()

if(PYTHON3 AND EXISTS "${SPOOFER_SRC_SYS}")
    add_custom_command(
        OUTPUT  "${SPOOFER_ENC}"
        COMMAND ${PYTHON3} "${ENCRYPT_SCRIPT}"
                --in            "${SPOOFER_SRC_SYS}"
                --out           "${SPOOFER_ENC}"
                --secret-header "${EMBED_SECRET_HDR}"
                --magic         DFSP
                --salt          dotafarm-sys-v1
                --info          sys-embed
        DEPENDS "${SPOOFER_SRC_SYS}" "${ENCRYPT_SCRIPT}" "${EMBED_SECRET_HDR}"
        COMMENT "Encrypting spoofer.sys for embedding"
        VERBATIM
    )
else()
    message(WARNING
        "SPOOFER_SRC_SYS not found at '${SPOOFER_SRC_SYS}' OR python missing — "
        "SPOOFER_SYS resource will be empty. HWID spoof in runtime will skip.")
endif()

if(PYTHON3 AND EXISTS "${KDU_SRC_EXE}")
    add_custom_command(
        OUTPUT  "${KDU_ENC}"
        COMMAND ${PYTHON3} "${ENCRYPT_SCRIPT}"
                --in            "${KDU_SRC_EXE}"
                --out           "${KDU_ENC}"
                --secret-header "${EMBED_SECRET_HDR}"
                --magic         DFSP
                --salt          dotafarm-exe-v1
                --info          exe-embed
        DEPENDS "${KDU_SRC_EXE}" "${ENCRYPT_SCRIPT}" "${EMBED_SECRET_HDR}"
        COMMENT "Encrypting kdu.exe for embedding"
        VERBATIM
    )
else()
    message(WARNING
        "KDU_SRC_EXE not found at '${KDU_SRC_EXE}' OR python missing — "
        "KDU_EXE resource will be empty. HWID spoof in runtime will skip.")
endif()

add_custom_target(EncryptResources
    DEPENDS "${ANDROMEDA_ENC}" "${PROXYHOOK_ENC}" "${SPOOFER_ENC}" "${KDU_ENC}"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources.rc")
    target_sources(${PROJECT_NAME} PRIVATE src/resources.rc)
    add_dependencies(${PROJECT_NAME} EncryptResources)
    message(STATUS "DotaFarm: linking embedded resources.rc (Andromeda + ProxyHook + Spoofer + KDU)")
endif()

# ── Test target: DotaMinifier unit tests (standalone, no gtest) ──
add_executable(DotaMinifierTest
    src/dota_minifier_test.cpp
    src/dota_minifier.cpp
    src/bot_dota_dir.cpp
    src/config.cpp
)
target_include_directories(DotaMinifierTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)
target_compile_definitions(DotaMinifierTest PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(DotaMinifierTest PRIVATE
    advapi32.lib
    shell32.lib
)

# ── Test target: pair_code codec unit tests (standalone, no gtest) ──
add_executable(pair_code_test
    src/pair_code_test.cpp
    src/pair_code.cpp
)
target_include_directories(pair_code_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)
target_compile_definitions(pair_code_test PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── Post-build: copy assets/ (vendored fonts + hero portraits) next to EXE ──
# Fonts MUST resolve from assets\fonts at runtime — we refuse to fall back
# to system fonts (Inter / Segoe / Consolas) because they're the AI-slop tell.
set(ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(EXISTS "${ASSETS_DIR}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ASSETS_DIR}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    )
endif()

# ── Post-build: copy dota2_minify_wrapper.py (Bundle G2) ──
# Wrapper script нужен в scripts/ next to EXE — там его ищет DotaMinifier
# RunPython(). При packaging через package.sh — vendor dota2_minify/ копируется
# отдельно туда же (см. comment в package.sh).
set(MINIFY_WRAPPER "${CMAKE_CURRENT_SOURCE_DIR}/../dota2_minify_wrapper.py")
if(EXISTS "${MINIFY_WRAPPER}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINIFY_WRAPPER}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/scripts/dota2_minify_wrapper.py"
    )
endif()

# ── Post-build: copy WinDivert DLL + driver next to EXE ──
set(WINDIVERT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proxydivert/deps/windivert")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/WinDivert.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/WinDivert.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/WinDivert64.sys"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/WinDivert64.sys"
)

# ── Post-build: copy sing-box + WinTUN next to EXE ──
set(SINGBOX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../singbox/deps")
if(EXISTS "${SINGBOX_DIR}/sing-box.exe" AND EXISTS "${SINGBOX_DIR}/wintun.dll")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SINGBOX_DIR}/sing-box.exe"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/sing-box.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SINGBOX_DIR}/wintun.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/wintun.dll"
    )
else()
    message(WARNING "sing-box.exe or wintun.dll not found in ${SINGBOX_DIR} — run fetch.sh")
endif()
