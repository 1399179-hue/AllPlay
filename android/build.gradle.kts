// AllPlay —— Android 宿主（Kotlin + NDK C++ core + JNI）
// core 是平台无关 C++（见 ../core），经 app/src/main/cpp 的 JNI 桥接入。
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
