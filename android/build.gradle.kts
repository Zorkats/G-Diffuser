plugins {
    id("com.android.application") version "9.3.2" apply false
}

// No org.jetbrains.kotlin.android here, and that is not an omission. AGP 9 has built-in Kotlin
// support and registers the `kotlin` extension itself, so applying the Kotlin plugin on top
// fails outright with "Cannot add extension with name 'kotlin'". The Kotlin version comes from
// AGP with this arrangement rather than being pinned here; app/build.gradle.kts still sets the
// bytecode target, which is the part that has to agree with compileOptions.
