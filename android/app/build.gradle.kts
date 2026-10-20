plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// 构建时把 MFi 身份注入 assets/offline-mfi（身份文件不提交到公开仓库）：
//   ALLPLAY_AUTH_ASSETS_DIR=<含 offline-mfi/identity.pk8 + certificate.p7b 的目录>
// 首启时服务会把 assets 里的身份释放到 files/identity，无需再手工 adb push。
val authAssetsDir = System.getenv("ALLPLAY_AUTH_ASSETS_DIR")
val generatedAuthAssets = layout.buildDirectory.dir("generated/authAssets").get().asFile
val copyAuthAssets = tasks.register<Copy>("copyAuthAssets") {
    if (authAssetsDir != null) {
        from(file("$authAssetsDir/offline-mfi")) { into("offline-mfi") }
        into(generatedAuthAssets)
    }
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
        versionCode = 2
        versionName = "0.1.1"

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

    signingConfigs {
        // 固定签名：CI 用 ANDROID_KEYSTORE_* 注入，保证新包能直接覆盖旧包
        // （否则每次构建都是新的临时 debug key，必须卸载重装）。
        create("release") {
            val path = System.getenv("ANDROID_KEYSTORE_PATH")
            if (path != null) {
                storeFile = file(path)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (System.getenv("ANDROID_KEYSTORE_PATH") != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }

    sourceSets["main"].assets.srcDir(generatedAuthAssets)

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

tasks.named("preBuild") { dependsOn(copyAuthAssets) }

dependencies {
    // androidx(appcompat/core-ktx/lifecycle) 在源码里未被引用，且 appcompat 1.7 要求 minSdk 21，
    // 已移除，只保留真正用到的 JmDNS（mDNS 广播/发现）。
    implementation("org.jmdns:jmdns:3.6.3")
}
