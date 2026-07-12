package ens.intellij

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.command.CommandEvent
import com.intellij.openapi.command.CommandListener
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import java.util.concurrent.atomic.AtomicBoolean

// Workspace edits from the language server (rename) can land in files without an
// open editor; those documents stay unsaved until the IDE's own autosave, so the
// server's file watcher sees the change late and diagnostics stay stale. Editor
// document listeners never fire for editor-less documents, so instead flush them
// after each finished command (refactorings run inside commands).
class EnsBackgroundDocumentSaver : ProjectActivity {
    override suspend fun execute(project: Project) {
        EnsBackgroundSaveListener.ensureRegistered()
    }
}

private object EnsBackgroundSaveListener : CommandListener {
    private val registered = AtomicBoolean(false)

    fun ensureRegistered() {
        if (!registered.compareAndSet(false, true)) return
        ApplicationManager.getApplication().messageBus.connect()
            .subscribe(CommandListener.TOPIC, this)
    }

    override fun commandFinished(event: CommandEvent) {
        val manager = FileDocumentManager.getInstance()
        val editorFactory = EditorFactory.getInstance()
        val pending = manager.unsavedDocuments.filter { document ->
            val file = manager.getFile(document)
            file != null && file.isInLocalFileSystem && file.extension == "ens" &&
                editorFactory.getEditors(document).isEmpty()
        }
        if (pending.isEmpty()) return
        ApplicationManager.getApplication().invokeLater {
            for (document in pending) {
                if (manager.isDocumentUnsaved(document)) {
                    manager.saveDocument(document)
                }
            }
        }
    }
}
