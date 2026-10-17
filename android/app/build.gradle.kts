plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.weiye.deskclip"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.weiye.deskclip"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
    }
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    packaging {
        resources.excludes += setOf("META-INF/versions/9/OSGI-INF/MANIFEST.MF", "META-INF/LICENSE*")
    }
}

dependencies {
    // P-256 point arithmetic for SPAKE2 (the platform JCA has no point addition).
    implementation("org.bouncycastle:bcprov-jdk18on:1.78.1")
    testImplementation("junit:junit:4.13.2")
    // android.jar's org.json is a stub on the JVM; use the real one in unit tests.
    testImplementation("org.json:json:20240303")
}
