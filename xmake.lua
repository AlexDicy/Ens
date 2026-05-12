add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm")
add_requires("libllvm", { configs = { lld = true } })
add_requires("libxml2", { configs = { runtimes = "MT" } })
add_requires("lsp-framework 1.3.1", { configs = { runtimes = "MT" } })

if is_plat("windows") then
    -- libllvm is MT only
    set_runtimes("MT")
end

includes("scripts/xmake_**.lua")

rule("link-llvm-libs")
    on_config(function (target)
        if not is_plat("macosx") then return end
        local tc = target:toolchain("llvm")
        if not tc then return end
        local sdkdir = tc:sdkdir()
        if not sdkdir then return end
        local libdir = path.join(sdkdir, "lib")
        if os.isdir(libdir) then
            target:add("linkdirs", libdir)
            target:add("rpathdirs", libdir)
        end
    end)


target("ens-frontend")
    set_kind("static")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("compiler/frontend/**.cpp")
    add_headerfiles("compiler/frontend/**.h")
    add_includedirs("compiler/frontend", { public = true })


target("ens-codegen")
    set_kind("static")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs", "gen-macos-sdk-stubs")
    add_files("compiler/codegen/**.cpp")
    add_headerfiles("compiler/codegen/**.h")
    add_includedirs("compiler/codegen", { public = true })
    add_deps("ens-frontend")
    add_packages("libllvm", "libxml2", { public = true })
    add_links("LLVMCGData")


target("ens")
    set_kind("binary")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("compiler/driver/**.cpp")
    add_headerfiles("compiler/driver/**.h")
    add_deps("ens-frontend", "ens-codegen")


target("ens-lsp")
    set_kind("binary")
    set_languages("cxx20")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("lsp/**.cpp")
    add_headerfiles("lsp/**.h")
    add_deps("ens-frontend")
    add_packages("lsp-framework")
