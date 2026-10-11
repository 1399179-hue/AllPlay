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

# 项目根（android/app/src/main/cpp 上溯 5 层）
get_filename_component(ALLPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# core（平台无关；含 find_package(OpenSSL/Threads)）
add_subdirectory("${ALLPLAY_ROOT}/core" core)

# POSIX TCP/网卡（NDK 提供 POSIX socket + getifaddrs）
add_library(allplay_posix_net STATIC
    "${ALLPLAY_ROOT}/platform/posix/net_posix.cpp")
target_include_directories(allplay_posix_net PUBLIC
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")

# JNI 桥：把 core::port 接口实现成对 Kotlin 服务对象的下调
add_library(allplay_jni SHARED jni_bridge.cpp)
target_include_directories(allplay_jni PRIVATE
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")
target_link_libraries(allplay_jni
    allplay_core
    allplay_posix_net
    log)
