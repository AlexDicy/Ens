add_rules("mode.debug", "mode.release")

add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm")

target("ens")
    set_kind("binary")
    set_languages("cxx17")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_packages("llvm")
