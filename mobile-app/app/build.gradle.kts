plugins {
    id("com.android.application")
}

android {
    namespace = "com.borg.nnscreen"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.borg.nnscreen"
        minSdk = 23
        targetSdk = 35
        versionCode = 5
        versionName = "1.4.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
