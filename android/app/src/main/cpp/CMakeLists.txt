# ============================================================================
# AllPlay Android 原生层：core（平台无关）+ POSIX 网卡/TCP + JNI 桥
# ============================================================================
# 依赖说明（唯一三方 = OpenSSL）：core 需要 OpenSSL。Android 无系统 OpenSSL，
# 需为 arm64-v8a / armeabi-v7a / x86_64 交叉编译一份（如 openssl-3.x NDK 构建），
# 并在 gradle 的 externalNativeBuild.cmake.arguments 里传：
#   -DOPENSSL_ROOT_DIR=<openssl-install-prefix>
# 蓝牙 / mDNS / 视频解码不走 C++ 库，经 JNI 调系统 API（BluetoothSocket / NsdManager / MediaCodec）。
cmake_minimum_required(VERSION 3.22)
project(allplay_jni LANGUAGES CXX)

# core 用 std::filesystem / 嵌套 namespace 等 C++17 特性；桌面顶层已设，安卓顶层也要设
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 项目根（android/app/src/main/cpp 上溯 5 层）
get_filename_component(ALLPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# OpenSSL 按 ABI 分目录交叉编译：<OPENSSL_BASE_DIR>/openssl-arm64 与 openssl-arm。
# 在 add_subdirectory(core)（内部 find_package(OpenSSL)）之前算好 OPENSSL_ROOT_DIR。
if(ANDROID AND DEFINED OPENSSL_BASE_DIR)
    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-arm")
    elseif(ANDROID_ABI STREQUAL "arm64-v8a")
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-arm64")
    else()
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-${ANDROID_ABI}")
    endif()
endif()

# core（平台无关；含 find_package(OpenSSL/Threads)）
add_subdirectory("${ALLPLAY_ROOT}/core" core)

# POSIX TCP/网卡（NDK 提供 POSIX socket + getifaddrs）
add_library(allplay_posix_net STATIC
    "${ALLPLAY_ROOT}/platform/posix/net_posix.cpp")
target_include_directories(allplay_posix_net PUBLIC
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")

# JNI 桥：把 core::port 接口实现成对 Kotlin 服务对象的下调
add_library(allplay_jni SHARED jni_bridge.cpp stdio_compat.cpp)
target_include_directories(allplay_jni PRIVATE
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")
target_link_libraries(allplay_jni
    allplay_core
    allplay_posix_net
    log)
