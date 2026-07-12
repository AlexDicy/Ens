package ens.intellij

import com.intellij.openapi.options.BoundConfigurable
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.dsl.builder.AlignX
import com.intellij.ui.dsl.builder.bindText
import com.intellij.ui.dsl.builder.panel
import com.redhat.devtools.lsp4ij.LanguageServerManager

class EnsSettingsConfigurable(private val project: Project) : BoundConfigurable("Ens") {
    override fun createPanel(): DialogPanel {
        val settings = EnsSettings.getInstance(project)
        return panel {
            row("Server path:") {
                textFieldWithBrowseButton()
                    .bindText(
                        { settings.state.serverPath.orEmpty() },
                        { settings.state.serverPath = it.ifBlank { null } },
                    )
                    .align(AlignX.FILL)
                    .comment("Path to the ens-lsp executable. When empty, the newest binary in the project's build folder is used.")
            }
        }
    }

    override fun apply() {
        super.apply()
        LanguageServerManager.getInstance(project).stop("ens")
        LanguageServerManager.getInstance(project).start("ens")
    }
}
