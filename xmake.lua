add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm 21.1.0")
add_requires("libllvm 21.1.0", { configs = { lld = true } })
add_requires("libxml2 v2.14.3", { configs = { runtimes = "MT" } })
add_requires("lsp-framework 1.3.1", { configs = { runtimes = "MT" } })

if is_plat("windows") then
    -- libllvm is MT only
    set_runtimes("MT")
    -- Silence the MSVC CRT's deprecation warning for standard C functions such
    -- as 'std::getenv'; the standard names are used deliberately.
    add_defines("_CRT_SECURE_NO_WARNINGS")
end

includes("scripts/xmake_**.lua")

rule("link-llvm-libs")
    on_config(function (target)
        if is_plat("macosx") then
            local tc = target:toolchain("llvm")
            if tc then
                local sdkdir = tc:sdkdir()
                if sdkdir then
                    local libdir = path.join(sdkdir, "lib")
                    if os.isdir(libdir) then
                        target:add("linkdirs", libdir)
                        target:add("rpathdirs", libdir)
                    end
                end
            end
        end

        -- bundle libllvm libraries skipping non-static dependencies
        local function find_libllvm(t)
            for _, pkg in ipairs(t:orderpkgs()) do
                if pkg:name() == "libllvm" then return pkg end
            end
            for _, dep in ipairs(t:orderdeps()) do
                local pkg = find_libllvm(dep)
                if pkg then return pkg end
            end
        end
        local pkg = find_libllvm(target)
        if pkg then
            local libdirs = table.wrap(pkg:get("linkdirs"))
            local function shared_only(link)
                local found_shared = false
                for _, dir in ipairs(libdirs) do
                    if os.isfile(path.join(dir, "lib" .. link .. ".a")) then
                        return false
                    end
                    if os.isfile(path.join(dir, "lib" .. link .. ".so")) then
                        found_shared = true
                    end
                end
                return found_shared
            end
            local filtered = {}
            for _, link in ipairs(table.wrap(pkg:get("links"))) do
                if not shared_only(link) then
                    table.insert(filtered, link)
                end
            end
            pkg:set("links", filtered)
        end
    end)


-- The language server's own front end: lexer, parser, CST and semantic analysis, in C++. It is
-- not the language's front end, which is written in Ens under selfhost/frontend.
target("lsp-frontend")
    set_kind("static")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_files("lsp/frontend/**.cpp")
    add_headerfiles("lsp/frontend/**.h")
    add_includedirs("lsp/frontend", { public = true })


-- The linker bridge `ens` links every Ens program through: one C entry point over lld's C++
-- drivers, shipped as a shared library beside LLVM-C. All link policy lives in selfhost/link.
target("ens-lld")
    set_kind("shared")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("runtime/lld/ens_lld.cpp")
    add_packages("libllvm", "libxml2")
    -- fix to have the flag appear later compared to add_links(...) for GNU ld
    add_shflags("-lLLVMCGData", { force = true })
    -- Only the two entry points leave this library. It statically links lld and the parts of LLVM
    -- lld needs, and a program that binds it also binds LLVM itself: any LLVM symbol exported here
    -- would resolve to this copy's own registries instead of that LLVM's. Windows exports only what
    -- the source marks, so this says the same thing for the platforms that would otherwise export
    -- everything.
    if is_plat("macosx") then
        add_shflags("-Wl,-exported_symbol,_ens_lld_link", "-Wl,-exported_symbol,_ens_lld_free",
            { force = true })
    elseif not is_plat("windows") then
        add_shflags("-Wl,--exclude-libs,ALL", { force = true })
        add_cxxflags("-fvisibility=hidden")
    end


-- The language server. `xmake test` never builds it, so a change anywhere under lsp/ is gated by
-- building this target by hand.
target("ens-lsp")
    set_kind("binary")
    set_languages("cxx20")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("lsp/server/**.cpp")
    add_headerfiles("lsp/server/**.h")
    add_deps("lsp-frontend")
    add_packages("lsp-framework")
