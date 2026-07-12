package ens.intellij

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.ProjectManager
import com.intellij.openapi.startup.ProjectActivity
import com.redhat.devtools.lsp4ij.LSPIJUtils
import com.redhat.devtools.lsp4ij.LanguageServerManager
import com.redhat.devtools.lsp4ij.ServerStatus
import org.eclipse.lsp4j.jsonrpc.Endpoint
import java.util.concurrent.atomic.AtomicBoolean

// The IDE modifies Ens documents that are not open in the LSP sense (rename
// workspace edits, undo in files without a tab). Those edits produce neither
// didChange nor a save, so forward the document text to the server directly;
// the server ignores reports for documents it already has open.
class EnsBackgroundDocumentForwarder : ProjectActivity {
    override suspend fun execute(project: Project) {
        EnsBackgroundChangeListener.ensureRegistered()
    }
}

private object EnsBackgroundChangeListener : DocumentListener {
    private val registered = AtomicBoolean(false)
    private val pending = mutableSetOf<Document>()

    fun ensureRegistered() {
        if (!registered.compareAndSet(false, true)) return
        EditorFactory.getInstance().eventMulticaster
            .addDocumentListener(this, ApplicationManager.getApplication())
    }

    override fun documentChanged(event: DocumentEvent) {
        val file = FileDocumentManager.getInstance().getFile(event.document) ?: return
        if (!file.isInLocalFileSystem || file.extension != "ens") return
        val firstPending = pending.isEmpty()
        pending.add(event.document)
        if (firstPending) ApplicationManager.getApplication().invokeLater { flushPending() }
    }

    private fun flushPending() {
        val documents = pending.toList()
        pending.clear()
        for (document in documents) {
            val file = FileDocumentManager.getInstance().getFile(document) ?: continue
            val params = mapOf(
                "uri" to LSPIJUtils.toUri(file).toASCIIString(),
                "text" to document.text,
            )
            for (project in ProjectManager.getInstance().openProjects) {
                if (project.isDisposed) continue
                val manager = LanguageServerManager.getInstance(project)
                if (manager.getServerStatus("ens") != ServerStatus.started) continue
                manager.getLanguageServer("ens").thenAccept { item ->
                    (item?.server as? Endpoint)?.request("ens/didChangeBackground", params)
                }
            }
        }
    }
}
