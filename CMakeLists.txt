# ============================================================================
# AllPlay —— 分层（ports & adapters）
# ============================================================================
#   core/           平台无关协议核心（唯一三方依赖 = OpenSSL）
#   platform/posix  Linux+macOS 适配（socket/avahi·dns_sd/bluez·IOBluetooth）
#   platform/windows Windows 适配（Winsock2，待做；当前只构建 core + 自测）
#   host/           桌面开发宿主（SDL2，仅开发验收用；可缺省跳过）
# 本文件随里程碑逐步长出 target。
cmake_minimum_required(VERSION 3.16)
project(AllPlay LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# core（平台无关；不含任何平台源/平台头）—— 见 core/CMakeLists.txt
# ---------------------------------------------------------------------------
add_subdirectory(core)

# ---------------------------------------------------------------------------
# 自测（只依赖 allplay_core，全平台：Linux/macOS/Windows 都能编）
# ---------------------------------------------------------------------------
add_executable(iap2_test tools/iap2_test.cpp)
target_link_libraries(iap2_test PRIVATE allplay_core)
target_compile_options(iap2_test PRIVATE -Wall -Wextra)

# M3 自测：加密层（HKDF/ChaCha/SRP/X25519/Ed25519/TLV8）
add_executable(m3_test tools/m3_test.cpp)
target_link_libraries(m3_test PRIVATE allplay_core)
target_compile_options(m3_test PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# 平台适配 + 桌面宿主：仅 Unix（Windows 的 Winsock2 适配层待做）
# ---------------------------------------------------------------------------
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)

    # platform/posix 适配（Linux + macOS）
    if(APPLE)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 17)
        set(POSIX_MDNS_SRC platform/posix/mdns_dns_sd.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_iobluetooth.mm)
        set_source_files_properties(platform/posix/bt_iobluetooth.mm
            PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
        set(POSIX_FRAMEWORKS "-framework IOBluetooth" "-framework CoreWLAN" "-framework Foundation")
        message(STATUS "蓝牙传输: IOBluetooth (macOS)")
    else()
        set(POSIX_MDNS_SRC platform/posix/mdns_avahi.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_bluez.cpp)
        set(POSIX_FRAMEWORKS "")
        pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)
        # BlueZ 传输层：libbluetooth 出 SDP 查询 + 裸 RFCOMM socket + HCI 取本机地址；
        # libdbus-1 用来枚举已配对设备（org.bluez）与当前 SSID（NetworkManager）。
        pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
        pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)
        message(STATUS "蓝牙传输: BlueZ (Linux，libbluetooth ${BLUEZ_VERSION})")
    endif()

    add_library(allplay_posix STATIC
        platform/posix/net_posix.cpp
        ${POSIX_MDNS_SRC}
        ${POSIX_BT_SRC}
    )
    target_include_directories(allplay_posix PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/platform)
    target_link_libraries(allplay_posix PUBLIC allplay_core ${POSIX_FRAMEWORKS})
    if(NOT APPLE)
        target_link_libraries(allplay_posix PUBLIC
            PkgConfig::AVAHI_CLIENT PkgConfig::BLUEZ PkgConfig::DBUS1)
    endif()
    target_compile_options(allplay_posix PRIVATE -Wall -Wextra)

    add_executable(core_selftest tools/core_selftest.cpp)
    target_link_libraries(core_selftest PRIVATE allplay_posix)
    target_compile_options(core_selftest PRIVATE -Wall -Wextra)

    # 桌面开发宿主（SDL2，可选：缺 SDL2/SDL2_ttf 时跳过）
    pkg_check_modules(SDL2    IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2TTF IMPORTED_TARGET SDL2_ttf)
    if(SDL2_FOUND AND SDL2TTF_FOUND)
        add_executable(allplay_host
            host/main.cpp
            host/statusbar.cpp
        )
        target_include_directories(allplay_host PRIVATE host)
        target_link_libraries(allplay_host PRIVATE PkgConfig::SDL2 PkgConfig::SDL2TTF)
        target_compile_options(allplay_host PRIVATE -Wall -Wextra)
        message(STATUS "桌面宿主 allplay_host: 启用")
    else()
        message(STATUS "桌面宿主 allplay_host: 跳过（缺 SDL2/SDL2_ttf）")
    endif()
else()
    # Windows：Winsock2 适配 + Win32 宿主（1280×720 窗口）
    add_library(allplay_windows STATIC
        platform/windows/net_windows.cpp
    )
    target_include_directories(allplay_windows PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/platform)
    target_link_libraries(allplay_windows PUBLIC allplay_core ws2_32 iphlpapi)
    target_compile_options(allplay_windows PRIVATE -Wall -Wextra)

    add_executable(allplay_windows_host
        host/windows_main.cpp
    )
    target_include_directories(allplay_windows_host PRIVATE host)
    target_link_libraries(allplay_windows_host PRIVATE allplay_windows)
    target_compile_options(allplay_windows_host PRIVATE -Wall -Wextra)
    message(STATUS "Windows 宿主 allplay_windows_host: 启用")
endif()
