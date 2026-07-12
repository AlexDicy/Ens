package ens.intellij

import com.intellij.openapi.application.ApplicationListener
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.command.CommandEvent
import com.intellij.openapi.command.CommandListener
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import java.util.concurrent.atomic.AtomicBoolean

// Workspace edits from the language server (rename) can land in files without an
// open editor; those documents stay unsaved until the IDE's own autosave, so the
// server's file watcher sees the change late and diagnostics stay stale. The edits
// are applied in plain write actions, not commands, so scan for unsaved editor-less
// Ens documents after both and flush them to disk.
class EnsBackgroundDocumentSaver : ProjectActivity {
    override suspend fun execute(project: Project) {
        EnsBackgroundSaveListener.ensureRegistered()
    }
}

private object EnsBackgroundSaveListener : CommandListener, ApplicationListener {
    private val log = logger<EnsBackgroundDocumentSaver>()
    private val registered = AtomicBoolean(false)
    private val scanScheduled = AtomicBoolean(false)

    fun ensureRegistered() {
        if (!registered.compareAndSet(false, true)) return
        val application = ApplicationManager.getApplication()
        application.messageBus.connect().subscribe(CommandListener.TOPIC, this)
        application.addApplicationListener(this, application)
        log.info("Ens background saver registered")
    }

    override fun commandFinished(event: CommandEvent) = scheduleScan()

    override fun afterWriteActionFinished(action: Any) = scheduleScan()

    private fun scheduleScan() {
        if (!scanScheduled.compareAndSet(false, true)) return
        ApplicationManager.getApplication().invokeLater {
            scanScheduled.set(false)
            saveEditorlessEnsDocuments()
        }
    }

    private fun saveEditorlessEnsDocuments() {
        val manager = FileDocumentManager.getInstance()
        val editorFactory = EditorFactory.getInstance()
        val pending = manager.unsavedDocuments.filter { document ->
            val file = manager.getFile(document)
            file != null && file.isInLocalFileSystem && file.extension == "ens" &&
                editorFactory.getEditors(document).isEmpty()
        }
        for (document in pending) {
            manager.saveDocument(document)
            log.info("Ens background saver: saved ${manager.getFile(document)?.path}")
        }
    }
}
