package ens.intellij

import com.intellij.openapi.project.Project
import com.intellij.openapi.util.SystemInfo
import java.nio.file.Files
import java.nio.file.Path

// Mirrors the server discovery of the VSCode extension: an explicit setting wins,
// otherwise the newest binary in the project's xmake build tree is used.
object EnsServerDiscovery {
    private val platforms = listOf(
        "windows" to "x64",
        "linux" to "x86_64",
        "macosx" to "arm64",
        "macosx" to "x86_64",
    )

    fun resolve(project: Project): Path? {
        val configured = EnsSettings.getInstance(project).state.serverPath
        if (!configured.isNullOrBlank()) return Path.of(configured)

        val basePath = project.basePath ?: return null
        val executableName = if (SystemInfo.isWindows) "ens-lsp.exe" else "ens-lsp"
        val candidates = platforms.flatMap { (os, arch) ->
            listOf("release", "debug").map { mode -> Path.of(basePath, "build", os, arch, mode, executableName) } +
                listOf(Path.of(basePath, "build", os, arch, executableName))
        }
        return candidates
            .filter { Files.isRegularFile(it) }
            .maxByOrNull { Files.getLastModifiedTime(it) }
    }
}
