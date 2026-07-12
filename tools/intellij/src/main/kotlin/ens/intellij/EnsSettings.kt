package ens.intellij

import com.intellij.openapi.components.BaseState
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.SimplePersistentStateComponent
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project

@Service(Service.Level.PROJECT)
@State(name = "EnsSettings", storages = [Storage("ens.xml")])
class EnsSettings : SimplePersistentStateComponent<EnsSettings.State>(State()) {
    class State : BaseState() {
        var serverPath by string()
    }

    companion object {
        fun getInstance(project: Project): EnsSettings = project.service()
    }
}
