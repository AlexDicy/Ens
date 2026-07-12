package ens.intellij

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import java.util.concurrent.atomic.AtomicBoolean

// Workspace edits from the language server (rename) can land in files without an
// open editor; those documents stay unsaved until the IDE's own autosave, so the
// server's file watcher sees the change late and diagnostics stay stale. Saving
// editor-less Ens documents immediately keeps the module graph in sync.
class EnsBackgroundDocumentSaver : ProjectActivity {
    override suspend fun execute(project: Project) {
        EnsBackgroundSaveListener.ensureRegistered()
    }
}

private object EnsBackgroundSaveListener : DocumentListener {
    private val registered = AtomicBoolean(false)

    fun ensureRegistered() {
        if (!registered.compareAndSet(false, true)) return
        EditorFactory.getInstance().eventMulticaster
            .addDocumentListener(this, ApplicationManager.getApplication())
    }

    override fun documentChanged(event: DocumentEvent) {
        val manager = FileDocumentManager.getInstance()
        val file = manager.getFile(event.document) ?: return
        if (!file.isInLocalFileSystem || file.extension != "ens") return
        if (EditorFactory.getInstance().getEditors(event.document).isNotEmpty()) return
        ApplicationManager.getApplication().invokeLater {
            if (manager.isDocumentUnsaved(event.document)) {
                manager.saveDocument(event.document)
            }
        }
    }
}
