package ens.intellij

import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.openapi.vfs.VirtualFileManager
import com.intellij.openapi.vfs.newvfs.BulkFileListener
import com.intellij.openapi.vfs.newvfs.events.VFileCreateEvent
import com.intellij.openapi.vfs.newvfs.events.VFileDeleteEvent
import com.intellij.openapi.vfs.newvfs.events.VFileEvent
import com.redhat.devtools.lsp4ij.LSPIJUtils
import com.redhat.devtools.lsp4ij.LanguageServerManager
import com.redhat.devtools.lsp4ij.ServerStatus
import org.eclipse.lsp4j.DidChangeWatchedFilesParams
import org.eclipse.lsp4j.FileChangeType
import org.eclipse.lsp4j.FileEvent

// The server registers a watcher for *.ens files so it can refresh diagnostics when
// imported files change on disk (rename edits in closed files). Client-side watcher
// delivery is not dependable, so forward the IDE's own VFS events directly.
class EnsFileWatchForwarder : ProjectActivity {
    override suspend fun execute(project: Project) {
        project.messageBus.connect().subscribe(VirtualFileManager.VFS_CHANGES, object : BulkFileListener {
            override fun after(events: List<VFileEvent>) {
                if (project.isDisposed) return
                val changes = events.mapNotNull { event ->
                    val file = event.file ?: return@mapNotNull null
                    if (!file.isInLocalFileSystem || file.extension != "ens") return@mapNotNull null
                    val type = when (event) {
                        is VFileCreateEvent -> FileChangeType.Created
                        is VFileDeleteEvent -> FileChangeType.Deleted
                        else -> FileChangeType.Changed
                    }
                    FileEvent(LSPIJUtils.toUri(file).toASCIIString(), type)
                }
                if (changes.isEmpty()) return
                val manager = LanguageServerManager.getInstance(project)
                if (manager.getServerStatus("ens") != ServerStatus.started) return
                manager.getLanguageServer("ens").thenAccept { item ->
                    item?.server?.workspaceService?.didChangeWatchedFiles(DidChangeWatchedFilesParams(changes))
                }
            }
        })
    }
}
