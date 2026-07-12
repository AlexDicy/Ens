plugins {
    kotlin("jvm") version "2.4.0"
    id("org.jetbrains.intellij.platform") version "2.18.1"
}

group = "ens"
version = "0.1.0"

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        intellijIdeaCommunity("2024.2.4")
        bundledPlugin("org.jetbrains.plugins.textmate")
        plugin("com.redhat.devtools.lsp4ij:0.20.1")
    }
}

kotlin {
    jvmToolchain(21)
}

intellijPlatform {
    buildSearchableOptions = false
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "242"
            untilBuild = provider { null }
        }
    }
}

tasks.runIde {
    jvmArgs("-Didea.trust.all.projects=true", "-Djb.consents.confirmation.enabled=false")
}

tasks.processResources {
    from("../grammar/ens.tmLanguage.json") { into("textmate/ens/syntaxes") }
    from("../grammar/language-configuration.json") { into("textmate/ens") }
}
