add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm 21.1.0")
add_requires("libllvm 21.1.0", { configs = { lld = true, clang = false, target_x86 = true, target_aarch64 = true } })
-- LLVM finds zlib and zstd wherever the machine keeps them, so ens-llvm links copies built here
-- instead, and `ens` does not load whichever ones a machine happens to have.
add_requires("zlib v1.3.2", { system = false, configs = { shared = false, runtimes = "MT" } })
add_requires("zstd v1.5.7", { system = false, configs = { shared = false, runtimes = "MT" } })
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
-- package definitions of our own that need more than one file, as libllvm's does; see the header
-- of each package there for why it is here
add_repositories("ens-repository scripts/repository")

-- A binary xmake links with the LLVM toolchain on macOS loads that toolchain's libc++, so its folder
-- is searched at link time and at run time.
rule("llvm-cxx-runtime")
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


-- Everything native that `ens` links, as one static archive: the linker bridge, the LLVM and lld
-- libraries it and the ens.llvm binding call, the libraries those depend on, the C++ runtime on
-- Linux and macOS, GCC's runtime library on Linux, and clang's builtins on Windows. `ens` is then
-- one executable that needs nothing beside it, and it holds one copy of LLVM. All link policy
-- lives in selfhost/link.
target("ens-llvm")
    set_kind("static")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_files("runtime/lld/ens_lld.cpp")
    add_packages("libllvm", "libxml2", "zlib", "zstd")
    -- For a target that names a C runtime, xmake has clang on Windows write a reference to clang's
    -- builtins library into every object, by a path relative to clang's own folders, which `ens`
    -- does not link through. xmake's rules give a target that names none the dynamic runtime while
    -- it is configured, so the runtime is cleared after them, clang is given the static one itself,
    -- and the builtins library is merged in below.
    if is_plat("windows") then
        add_cxxflags("-fms-runtime-lib=static", { force = true })
        on_config(function (target)
            target:set("runtimes", nil)
        end)
    end
    after_link(function (target)
        local archives = {}
        local seen = {}
        local function add(file)
            if not seen[file] then
                seen[file] = true
                table.insert(archives, file)
            end
        end
        local function staticArchive(dirs, link)
            for _, dir in ipairs(dirs) do
                for _, name in ipairs({"lib" .. link .. ".a", link .. ".lib"}) do
                    local file = path.join(dir, name)
                    if os.isfile(file) then
                        return file
                    end
                end
            end
        end
        local provided = {}
        local missing = {}
        -- the Windows LLVM package also carries LLVM-C, the import library of its DLL, and every
        -- LLVM names LTO and Remarks, which it builds only as shared libraries: C interfaces over
        -- component archives merged here, and ones `ens` binds none of.
        local sharedOnly = {["LLVM-C"] = true, LTO = true, Remarks = true}
        local function addPackage(package)
            local dirs = table.wrap(package:get("linkdirs"))
            for _, link in ipairs(table.wrap(package:get("links"))) do
                if package:name() ~= "libllvm" or not sharedOnly[link] then
                    local file = staticArchive(dirs, link)
                    if file then
                        add(file)
                        provided[link] = true
                    else
                        table.insert(missing, {package = package:name(), link = link, dirs = dirs})
                    end
                end
            end
            -- the LLVM package's list of links leaves some of its own components out, such as
            -- LLVMCGData, so every component archive in its folders is taken.
            if package:name() == "libllvm" then
                for _, dir in ipairs(dirs) do
                    for _, pattern in ipairs({"libLLVM*.a", "liblld*.a", "LLVM*.lib", "lld*.lib"}) do
                        for _, file in ipairs(os.files(path.join(dir, pattern))) do
                            if path.basename(file) ~= "LLVM-C" then
                                add(file)
                            end
                        end
                    end
                end
            end
        end
        for _, package in ipairs(target:orderpkgs()) do
            addPackage(package)
        end
        -- what `ens` links by name, through the native llvm declaration or by default.
        local systemLibraries = {m = true, pthread = true, dl = true, rt = true, c = true}
        for _, entry in ipairs(missing) do
            if not provided[entry.link] and not systemLibraries[entry.link] then
                local searched = #entry.dirs == 0
                    and "it names no folder to find it in, as a library found on the system does"
                    or "no static archive of it is in " .. table.concat(entry.dirs, ", ")
                raise("package '%s' links '%s', but %s, so ens-llvm cannot hold it. Require it " ..
                    "with 'system = false' and 'shared = false', as zlib is", entry.package,
                    entry.link, searched)
            end
        end
        if target:is_plat("macosx") then
            local llvm = target:toolchain("llvm")
            local folder = llvm and llvm:sdkdir() and path.join(llvm:sdkdir(), "lib")
            for _, name in ipairs({"libc++.a", "libc++abi.a"}) do
                local file = folder and path.join(folder, name)
                if not file or not os.isfile(file) then
                    raise("the LLVM toolchain has no '%s' to link `ens` with", name)
                end
                add(file)
            end
        elseif target:is_plat("windows") then
            -- the builtins of the target's processor: the toolchain's own when it is of that
            -- processor, and otherwise the libllvm package's, which LLVM's release archive for
            -- arm64 ships beside its libraries
            local architecture = target:is_arch("arm64") and "aarch64" or "x86_64"
            local name = "clang_rt.builtins-" .. architecture .. ".lib"
            local roots = {}
            local llvm = target:toolchain("llvm")
            if llvm and llvm:sdkdir() then
                table.insert(roots, llvm:sdkdir())
            end
            for _, package in ipairs(target:orderpkgs()) do
                if package:name() == "libllvm" then
                    table.insert(roots, package:installdir())
                end
            end
            local file = nil
            for _, root in ipairs(roots) do
                file = file or os.files(path.join(root, "lib", "clang", "*", "lib", "windows",
                    name))[1]
            end
            if not file then
                raise("neither the LLVM toolchain nor the libllvm package has '%s' to link `ens` " ..
                    "with", name)
            end
            add(file)
        elseif target:is_plat("linux") then
            local compiler = target:tool("cxx")
            local printed = os.iorunv(compiler, {"-print-file-name=libstdc++.a"}):trim()
            if not os.isfile(printed) then
                raise("'%s' has no 'libstdc++.a' to link `ens` with", compiler)
            end
            add(printed)
            local libgcc = os.iorunv(compiler, {"-print-libgcc-file-name"}):trim()
            if not os.isfile(libgcc) then
                raise("'%s' has no '%s' to link `ens` with", compiler, path.filename(libgcc))
            end
            add(libgcc)
        end
        -- llvm-ar reads an MRI script on every platform, so one merge serves every archive format.
        -- The archive is made again from the bridge's objects each time, never from the archive a
        -- previous merge left behind, so nothing is added twice.
        local llvm = target:toolchain("llvm")
        local suffix = is_host("windows") and ".exe" or ""
        local archiver = llvm and path.join(llvm:bindir(), "llvm-ar" .. suffix)
        if not archiver or not os.isfile(archiver) then
            raise("the LLVM toolchain has no llvm-ar to merge ens-llvm with")
        end
        local merged = target:targetfile() .. ".merging"
        local script = target:targetfile() .. ".mri"
        local lines = {"create " .. merged}
        for _, object in ipairs(target:objectfiles()) do
            table.insert(lines, "addmod " .. object)
        end
        for _, archive in ipairs(archives) do
            table.insert(lines, "addlib " .. archive)
        end
        table.insert(lines, "save")
        table.insert(lines, "end")
        io.writefile(script, table.concat(lines, "\n") .. "\n")
        os.tryrm(merged)
        os.vrunv(archiver, {"-M"}, {stdin = script})
        os.mv(merged, target:targetfile())
        os.rm(script)
    end)


-- The language server. The ens_lsp job of `xmake test` skips it while a running editor holds the
-- binary, so a change anywhere under lsp/ is gated by building this target by hand.
target("ens-lsp")
    set_kind("binary")
    set_languages("cxx20")
    set_toolchains("@llvm")
    add_rules("llvm-cxx-runtime")
    add_files("lsp/server/**.cpp")
    add_headerfiles("lsp/server/**.h")
    add_deps("lsp-frontend")
    add_packages("lsp-framework")
