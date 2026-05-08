add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm")
add_requires("libllvm", { configs = { lld = true } })
add_requires("libxml2", { configs = { runtimes = "MT" } })

target("ens")
    set_kind("binary")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_packages("libllvm", "libxml2")
    add_links("LLVMCGData")
    
    if is_plat("windows") then
        -- libllvm is MT only
        set_runtimes("MT")
    end
