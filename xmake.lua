add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("libllvm")

target("ens")
    set_kind("binary")
    set_languages("cxx17")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_packages("libllvm")
    
    if is_plat("windows") then
        -- libllvm is MT only
        set_runtimes("MT")
    end
