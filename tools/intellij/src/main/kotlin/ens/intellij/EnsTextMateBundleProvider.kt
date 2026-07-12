package ens.intellij

import com.intellij.openapi.application.PathManager
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider.PluginBundle

// The TextMate engine reads bundles from the filesystem, so the resources are
// extracted from the plugin jar to a stable directory on every IDE start.
class EnsTextMateBundleProvider : TextMateBundleProvider {
    private val bundleResources = listOf(
        "package.json",
        "language-configuration.json",
        "syntaxes/ens.tmLanguage.json",
    )

    override fun getBundles(): List<PluginBundle> {
        val bundleDirectory = Path.of(PathManager.getSystemPath(), "ens-textmate")
        for (resource in bundleResources) {
            val destination = bundleDirectory.resolve(resource)
            Files.createDirectories(destination.parent)
            val stream = javaClass.classLoader.getResourceAsStream("textmate/ens/$resource") ?: return emptyList()
            stream.use { Files.copy(it, destination, StandardCopyOption.REPLACE_EXISTING) }
        }
        return listOf(PluginBundle("Ens", bundleDirectory))
    }
}
