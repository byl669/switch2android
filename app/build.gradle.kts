plugins { id("com.android.application") }
val payloadAssets = providers.gradleProperty("s2aAssets")
val outputRoot = providers.gradleProperty("s2aBuildDir")
if (outputRoot.isPresent) layout.buildDirectory.set(file(outputRoot.get() + "/app"))
// CMake defaults S2A_GRAPHICS_BACKEND to "ot2", the reduced replay used by the
// contract tests. Every title this host actually runs needs the d2r engine, which
// owns the swapchain and the NVN translation, so the setting is passed here rather
// than left to a default that produces an APK drawing nothing but a cleared frame.
// Keep it overridable for a backend experiment: -Ps2aBackend=ot2.
val graphicsBackend = providers.gradleProperty("s2aBackend").getOrElse("d2r")
val licenseAssets = layout.buildDirectory.dir("generated/licenseAssets")
val copyNotices = tasks.register<Copy>("copyNotices") {
    from(rootProject.file("THIRD_PARTY_NOTICES.md"))
    into(licenseAssets)
    rename { "THIRD_PARTY_NOTICES.txt" }
}
tasks.named("preBuild").configure { dependsOn(copyNotices) }
android {
    sourceSets.getByName("main").assets.srcDir(licenseAssets.get().asFile)
    buildFeatures { resValues = true }
    namespace = "dev.ot2.port"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        applicationId = providers.gradleProperty("s2aAppId").getOrElse("org.switch2android.host")
        minSdk = 33
        targetSdk = 36
        versionCode = providers.gradleProperty("s2aVersionCode").getOrElse("1").toInt()
        // The host reached its first playable milestone at 1.0.0; the revision
        // number is what the installer compares, so it is supplied per build.
        versionName = providers.gradleProperty("s2aVersionName").getOrElse("1.0.0")
        resValue("string", "app_name", providers.gradleProperty("s2aName").getOrElse("Switch2Android"))
        testInstrumentationRunner = "dev.ot2.port.BundleImportInstrumentation"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_static" } }
        externalNativeBuild { cmake { arguments += "-DS2A_GRAPHICS_BACKEND=$graphicsBackend" } }
    }
    externalNativeBuild { cmake {
        path = file("../native/CMakeLists.txt"); version = "3.22.1"
        if (outputRoot.isPresent) buildStagingDirectory = file(outputRoot.get() + "/cxx")
    } }
    if (payloadAssets.isPresent) sourceSets.getByName("main").assets.srcDir(payloadAssets.get())
    androidResources { noCompress += "s2a" }
    buildTypes {
        getByName("release") {
            // Personal builds use the local Android debug signing key, but Release code
            // remains optimized and non-debuggable. Keep this key for upgrades.
            signingConfig = signingConfigs.getByName("debug")
            isDebuggable = true
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
