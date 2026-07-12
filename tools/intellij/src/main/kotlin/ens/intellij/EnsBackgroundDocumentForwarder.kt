package ens.intellij

import com.intellij.openapi.Disposable
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.Service
import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.fileEditor.FileEditorManager
import com.intellij.openapi.project.ProjectManager
import com.redhat.devtools.lsp4ij.LSPIJUtils
import com.redhat.devtools.lsp4ij.LanguageServerManager
import com.redhat.devtools.lsp4ij.ServerStatus
import org.eclipse.lsp4j.jsonrpc.Endpoint

// The IDE modifies Ens documents that are not open in the LSP sense (rename
// workspace edits, undo in files without a tab). Those edits produce neither
// didChange nor a save, so forward the document text to the server directly;
// the server ignores reports for documents it already has open. A disposable
// service, so dynamic plugin unload detaches the listener.
@Service(Service.Level.APP)
class EnsBackgroundDocumentForwarder : DocumentListener, Disposable {
    private val pending = mutableSetOf<Document>()

    init {
        EditorFactory.getInstance().eventMulticaster.addDocumentListener(this, this)
    }

    override fun documentChanged(event: DocumentEvent) {
        val file = FileDocumentManager.getInstance().getFile(event.document) ?: return
        if (!file.isInLocalFileSystem || file.extension != "ens") return
        if (isFocusedDocument(event.document)) return
        val firstPending = pending.isEmpty()
        pending.add(event.document)
        if (firstPending) ApplicationManager.getApplication().invokeLater { flushPending() }
    }

    // The focused editor's document is always open in the LSP sense; its didChange
    // is authoritative and forwarding it would only race that channel.
    private fun isFocusedDocument(document: Document): Boolean {
        return ProjectManager.getInstance().openProjects.any { project ->
            !project.isDisposed &&
                FileEditorManager.getInstance(project).selectedTextEditor?.document == document
        }
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

    override fun dispose() {}
}
