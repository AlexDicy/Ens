add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm")
add_requires("libllvm", { configs = { lld = true } })
add_requires("libxml2", { configs = { runtimes = "MT" } })
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
    -- fix to have the flag appear later comapred to add_links(...) for GNU ld
    add_ldflags("-lLLVMCGData", { public = true, force = true })


-- The reference compiler. It builds the seed the Ens-written compiler is bootstrapped from and
-- gates the tests/ fixtures; the user-facing `ens` command is the Ens-written one.
target("ens-ref")
    set_kind("binary")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("compiler/driver/**.cpp")
    add_headerfiles("compiler/driver/**.h")
    add_deps("ens-frontend", "ens-codegen")


-- The linker bridge the Ens-written compiler links through: one C entry point over lld's C++
-- drivers, shipped as a shared library beside LLVM-C so it outlives the C++ compiler.
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


target("ens-lsp")
    set_kind("binary")
    set_languages("cxx20")
    set_toolchains("@llvm")
    add_rules("link-llvm-libs")
    add_files("lsp/**.cpp")
    add_headerfiles("lsp/**.h")
    add_deps("ens-frontend")
    add_packages("lsp-framework")
