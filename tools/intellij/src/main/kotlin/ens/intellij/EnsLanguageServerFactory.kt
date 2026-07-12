package ens.intellij

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.project.Project
import com.redhat.devtools.lsp4ij.LanguageServerFactory
import com.redhat.devtools.lsp4ij.server.CannotStartProcessException
import com.redhat.devtools.lsp4ij.server.ProcessStreamConnectionProvider
import com.redhat.devtools.lsp4ij.server.StreamConnectionProvider
import java.nio.file.Files

class EnsLanguageServerFactory : LanguageServerFactory {
    override fun createConnectionProvider(project: Project): StreamConnectionProvider =
        EnsStreamConnectionProvider(project)
}

private class EnsStreamConnectionProvider(private val project: Project) : ProcessStreamConnectionProvider() {
    init {
        val serverPath = EnsServerDiscovery.resolve(project)
        if (serverPath != null && Files.isRegularFile(serverPath)) {
            commands = listOf(serverPath.toString())
            workingDirectory = project.basePath
        }
    }

    override fun start() {
        if (commands.isNullOrEmpty()) {
            NotificationGroupManager.getInstance()
                .getNotificationGroup("Ens")
                .createNotification(
                    "ens-lsp not found",
                    "Run \"xmake build ens-lsp\" in the project root, or set the server path in Settings | Languages & Frameworks | Ens.",
                    NotificationType.ERROR,
                )
                .notify(project)
            throw CannotStartProcessException("ens-lsp executable not found")
        }
        super.start()
    }
}
