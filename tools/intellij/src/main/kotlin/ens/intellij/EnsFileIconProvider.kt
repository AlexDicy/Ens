package ens.intellij

import com.intellij.ide.FileIconProvider
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.IconLoader
import com.intellij.openapi.vfs.VirtualFile
import javax.swing.Icon

class EnsFileIconProvider : FileIconProvider {
    override fun getIcon(file: VirtualFile, flags: Int, project: Project?): Icon? {
        return if (!file.isDirectory && file.extension.equals("ens", ignoreCase = true)) FILE_ICON else null
    }

    private companion object {
        val FILE_ICON: Icon = IconLoader.getIcon("/icons/ensFile.svg", EnsFileIconProvider::class.java)
    }
}
