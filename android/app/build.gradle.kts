plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.allplay.app"
    compileSdk = 34
    // NDK r25c：最后一个支持 API 19（Android 4.4）的版本（r26 起最低只到 21）
    ndkVersion = "25.2.9519653"

    defaultConfig {
        applicationId = "com.allplay.app"
        // 支持 Android 4.4（API 19）。注意：这台目标盒子是 32 位 armv7l，
        // abiFilters 目前仍是 arm64-v8a，armeabi-v7a 的 OpenSSL 交叉编译另行处理。
        minSdk = 19
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 目标盒子是 32 位 armv7l，需 armeabi-v7a；arm64-v8a 保留给其他设备。
            // 两者都要各自的 OpenSSL 交叉编译（见 env.sh 的 OPENSSL_BASE_DIR）。
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                // core 唯一三方依赖 = OpenSSL；按 ABI 分目录（openssl-arm64 / openssl-arm），
                // 由 cpp/CMakeLists.txt 依据 ANDROID_ABI 拼出具体 OPENSSL_ROOT_DIR。
                arguments += "-DANDROID_STL=c++_static"
                System.getenv("OPENSSL_BASE_DIR")?.let { arguments += "-DOPENSSL_BASE_DIR=$it" }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
}

dependencies {
    // androidx(appcompat/core-ktx/lifecycle) 在源码里未被引用，且 appcompat 1.7 要求 minSdk 21，
    // 已移除，只保留真正用到的 JmDNS（mDNS 广播/发现）。
    implementation("org.jmdns:jmdns:3.6.3")
}
