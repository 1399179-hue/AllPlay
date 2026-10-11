plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.allplay.app"
    compileSdk = 34
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.allplay.app"
        minSdk = 28
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 本机只交叉编译了 arm64-v8a 的 OpenSSL；其他 ABI 需各自编译 OpenSSL 后再开
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                // core 唯一三方依赖 = OpenSSL（arm64-v8a 交叉编译版，见 env.sh 的 OPENSSL_ROOT_DIR）
                arguments += "-DANDROID_STL=c++_static"
                System.getenv("OPENSSL_ROOT_DIR")?.let { arguments += "-DOPENSSL_ROOT_DIR=$it" }
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
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.lifecycle:lifecycle-service:2.8.3")
    implementation("org.jmdns:jmdns:3.6.3")
}
