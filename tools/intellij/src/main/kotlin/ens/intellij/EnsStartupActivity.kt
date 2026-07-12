package ens.intellij

import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity

class EnsStartupActivity : ProjectActivity {
    override suspend fun execute(project: Project) {
        project.service<EnsFileWatchForwarder>()
        service<EnsBackgroundDocumentForwarder>()
    }
}
