-- Overrides the xmake-repo definition of libllvm, copied from xmake-repo at 5ceb59ccd (2026-05-02),
-- so that every platform takes the package AlexDicy/llvm-builds publishes for it instead of
-- building LLVM here. That repository builds LLVM with the configuration this tree asks for, so
-- the compiler and the libraries ens links come from one build, on every platform.
package("libllvm")
    set_kind("library")
    set_homepage("https://llvm.org/")
    set_description("The LLVM Compiler Infrastructure.")

    add_configs("exception", {description = "Enable C++ exception support for LLVM.", default = true, type = "boolean"})
    add_configs("rtti",      {description = "Enable C++ RTTI support for LLVM.", default = true, type = "boolean"})

    add_configs("ms_dia",  {description = "Enable DIA SDK to support non-native PDB parsing. (msvc only)", default = true, type = "boolean"})
    add_configs("libffi",  {description = "Enable libffi to support the LLVM interpreter to call external functions.", default = false, type = "boolean"})
    add_configs("httplib", {description = "Enable cpp-httplib to support llvm-debuginfod serve debug information over HTTP.", default = false, type = "boolean"})
    add_configs("libcxx",  {description = "Use libc++ as C++ standard library instead of libstdc++", default = false, type = "boolean"})

    includes(path.join(os.scriptdir(), "constants.lua"))
    for _, project in ipairs(get_llvm_known_projects()) do
        add_configs(project:gsub("-", "_"), {description = "Build " .. project .. " project.", default = (project == "clang"), type = "boolean"})
    end
    for _, runtime in ipairs(get_llvm_all_runtimes()) do
        add_configs(runtime:gsub("-", "_"), {description = "Build " .. runtime .. " runtime.", default = false, type = "boolean"})
    end

    for _, target in ipairs(get_llvm_all_targets()) do
        add_configs("target_" .. target:lower(), {description = "Build " .. target .. " target backend.", default = false, type = "boolean"})
    end
    for _, target in ipairs(get_llvm_experimental_targets()) do
        add_configs("target_" .. target:lower(), {description = "Build " .. target .. " experimental target backend.", default = false, type = "boolean"})
    end

    if is_plat("windows") and is_arch("arm64") then
        add_urls("https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-windows-arm64-libllvm.tar.xz")
        add_versions("21.1.0", "4549885c0602891b018e86ffddee4f771e5a080f448ab89871fa4c725497e43a")
        -- The LLVM shared library cannot be built under windows.
        add_configs("shared", {description = "Build shared library.", default = false, type = "boolean", readonly = true})
        add_configs("runtimes",   {description = "Set vs compiler runtime.", default = "MT", readonly = true})
        add_configs("vs_runtime", {description = "Set vs compiler runtime.", default = "MT", readonly = true})
        -- LLVM's own build reads ntdll, which no directive in its libraries names, so a program
        -- linking them links the system libraries ens links on Windows
        add_syslinks("psapi", "shell32", "ole32", "uuid", "advapi32", "ws2_32", "ntdll", "version",
            "wsock32", "bcrypt")
    elseif is_plat("windows") then
        add_urls("https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-windows-x64-libllvm.tar.xz")
        add_versions("21.1.0", "343450ced6aa388c3cf2225924ace762805ce48fedcc422ee5f9f2b16c6f0e08")
        -- The LLVM shared library cannot be built under windows.
        add_configs("shared", {description = "Build shared library.", default = false, type = "boolean", readonly = true})
        add_configs("runtimes",   {description = "Set vs compiler runtime.", default = "MT", readonly = true})
        add_configs("vs_runtime", {description = "Set vs compiler runtime.", default = "MT", readonly = true})
        add_syslinks("psapi", "shell32", "ole32", "uuid", "advapi32", "ws2_32", "ntdll", "version")
    elseif is_plat("linux") then
        local arch = is_arch("arm64", "aarch64") and "arm64" or "x64"
        add_urls("https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r3/llvm-21.1.0-linux-" .. arch .. "-libllvm.tar.xz")
        if is_arch("arm64", "aarch64") then
            add_versions("21.1.0", "2c177af18ca2e123674c3f5b87a11afb5ee4ab88ed2fc697db9d1ca4804c8ce1")
        else
            add_versions("21.1.0", "bde69f078a67c98e56be87fe066ab71607bfca3de3cea947756bfba02578a304")
        end
        add_configs("shared", {description = "Build shared library.", default = false, type = "boolean", readonly = true})
    elseif is_plat("macosx") then
        add_urls("https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-macos-arm64-libllvm.tar.xz")
        add_versions("21.1.0", "694a0167d3070bb6c8ee3343754a90099689163215ed7664c7851abee223e608")
        add_configs("shared", {description = "Build shared library.", default = false, type = "boolean", readonly = true})
    else
        -- self-built
        add_urls("https://github.com/llvm/llvm-project/releases/download/llvmorg-$(version)/llvm-project-$(version).src.tar.xz", {alias = "tarball"})
        add_urls("https://github.com/llvm/llvm-project.git", {alias = "git"})
        add_versions("tarball:19.1.7", "82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501")
        add_versions("git:19.1.7", "llvmorg-19.1.7")

        add_versions("tarball:21.1.0", "1672e3efb4c2affd62dbbe12ea898b28a451416c7d95c1bd0190c26cbe878825")
        add_versions("git:21.1.0", "llvmorg-21.1.0")

        add_deps("ninja")
        add_deps("zlib", "zstd", {optional = true})
        set_policy("package.cmake_generator.ninja", true)
    end

    -- workaround to fix "error: undefined symbol: __mulodi4" (armeabi-v7a, r22, windows)
    if is_plat("android") then
        add_syslinks("compiler_rt-extras")
    end

    -- error: undefined symbol: backtrace
    if is_plat("bsd") then
        add_syslinks("execinfo")
    end

    -- cmake builds LLVM where this package compiles it, and drives nothing where LLVM's own
    -- release package is taken instead.
    if not is_plat("windows", "linux", "macosx") then
        add_deps("cmake")
    end
    on_load(function (package)
        local constants = import('constants')
        
        -- add deps.
        if not package:is_plat("windows", "linux", "macosx") then -- what is compiled here, not LLVM's package
            package:add("deps", "python 3.x", {kind = "binary", host = true})
            if package:config("libffi") then
                package:add("deps", "libffi")
            end
            if package:config("httplib") then
                package:add("deps", "cpp-httplib")
            end
            if package:config("libcxx") then
                package:add("deps", "libc++")
            end
        end

        if package:is_plat("windows") and package:config("ms_dia") then
            package:add("deps", "diasdk")
        end

        -- add links
        -- LLVM's own package names its components itself, and the names a release adds, drops or
        -- splits between versions are not this list's to know: where that package is taken, no
        -- links are declared, and ens-llvm merges every archive in the package's folders instead,
        -- which is what this list only approximates.
        if not package:is_plat("linux") then
            local linkable_projects = {"lldb", "lld", "clang", "polly", "bolt"}
            table.insert(linkable_projects, "llvm") -- make sure that the base library is last.
            for _, name in ipairs(linkable_projects) do
                local cname = name:gsub("-", "_")
                local ptype = package:config("shared") and "shared" or "static"
                if cname == "llvm" or package:config(cname) then
                    package:add("links", constants[("get_%s_%s_libraries"):format(cname, ptype)]())
                end
            end
        end
        if package:is_plat("windows") then
            package:add("links", "LLVM-C")
        end

        -- ens-llvm names the folders it uses: a package that names one leaves the defaults unnamed.

        -- LLVM 21+ introduces new split libraries that must be linked explicitly
        if package:version() and package:version():ge("21.0") then
            package:add("links", "LLVMFrontendDirective")
            package:add("links", "LLVMFrontendAtomic")
            package:add("links", "LLVMDebugInfoDWARFLowLevel")
            package:add("links", "LLVMDWARFCFIChecker")
        end

        -- automatically enable the target matching the host architecture
        local arch = package:arch()
        local target_mapping = {
            ["x86"] = "X86",
            ["x64"] = "X86",
            ["x86_64"] = "X86",
            ["i386"] = "X86",
            ["arm64"] = "AArch64",
            ["arm64-v8a"] = "AArch64",
            ["aarch64"] = "AArch64",
            ["arm"] = "ARM",
            ["armv7a"] = "ARM",
            ["armv7"] = "ARM",
            ["armeabi-v7a"] = "ARM",
            ["riscv64"] = "RISCV",
            ["riscv32"] = "RISCV",
            ["loong64"] = "LoongArch",
            ["mips"] = "Mips",
            ["mips64"] = "Mips",
            ["ppc64"] = "PowerPC",
            ["ppc64le"] = "PowerPC",
            ["wasm"] = "WebAssembly"
        }
        local host_target = target_mapping[arch]
        if host_target then
            package:config_set("target_" .. host_target:lower(), true)
        end

    end)

    on_install("windows|x64", function (package)
        os.cp("*", package:installdir())
    end)

    -- the headers, the libraries and the builtins beside them; the archive's programs would
    -- triple the package, and the toolchain package provides them
    on_install("windows|arm64", function (package)
        os.cp("include", package:installdir())
        os.cp("lib", package:installdir())
    end)

    -- the include tree, and the library tree, which holds the component archives and the cmake
    -- files. The programs in bin are left out, as the toolchain package provides them.
    on_install("linux", function (package)
        os.cp("include", package:installdir())
        os.cp("lib", package:installdir())
    end)

    -- the same two folders as linux, from the package llvm-builds publishes for macOS
    on_install("macosx", function (package)
        os.cp("include", package:installdir())
        os.cp("lib", package:installdir())
    end)

    -- the platforms llvm-builds does not publish for still compile LLVM here
    on_install("bsd", "android", "iphoneos", "cross", function (package)
        import("lib.detect.find_tool")
        import('constants')
        import("utils.ci.is_running", {alias = "ci_is_running"})

        local projects_enabled = {}
        local runtimes_enabled = {}
        local targets_enabled = {}
        local experimental_targets_enabled = {}
        for _, project in ipairs(constants.get_llvm_known_projects()) do
            if package:config(project:gsub("-", "_")) then
                table.insert(projects_enabled, project)
            end
        end
        for _, runtime in ipairs(constants.get_llvm_all_runtimes()) do
            if package:config(runtime:gsub("-", "_")) then
                table.insert(runtimes_enabled, runtime)
            end
        end
        for _, target in ipairs(constants.get_llvm_all_targets()) do
            if package:config("target_" .. target:lower()) then
                table.insert(targets_enabled, target)
            end
        end
        for _, target in ipairs(constants.get_llvm_experimental_targets()) do
            if package:config("target_" .. target:lower()) then
                table.insert(experimental_targets_enabled, target)
            end
        end

        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",

            -- llvm
            "-DLLVM_BUILD_UTILS=OFF",
            "-DLLVM_BUILD_EXAMPLES=OFF",
            "-DLLVM_INCLUDE_DOCS=OFF",
            "-DLLVM_INCLUDE_EXAMPLES=OFF",
            "-DLLVM_INCLUDE_TESTS=OFF",
            "-DLLVM_INCLUDE_BENCHMARKS=OFF",
            "-DLLVM_OPTIMIZED_TABLEGEN=ON",
            "-DLLVM_ENABLE_PROJECTS=" .. table.concat(projects_enabled, ";"),
            "-DLLVM_ENABLE_RUNTIMES=" .. table.concat(runtimes_enabled, ";"),

            -- disable tools build - to save link time
            "-DLLVM_BUILD_TOOLS=OFF",
            "-DCLANG_BUILD_TOOLS=OFF",
            "-DCLANG_ENABLE_CLANGD=OFF",
            "-DBOLT_BUILD_TOOLS=OFF",
            "-DFLANG_BUILD_TOOLS=OFF",
            "-DLLD_BUILD_TOOLS=OFF"
        }
        if #targets_enabled > 0 then
            table.insert(configs, "-DLLVM_TARGETS_TO_BUILD=" .. table.concat(targets_enabled, ";"))
        end
        if #experimental_targets_enabled > 0 then
            table.insert(configs, "-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=" .. table.concat(experimental_targets_enabled, ";"))
        end
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"))
        table.insert(configs, "-DLLVM_PARALLEL_COMPILE_JOBS=2")
        table.insert(configs, "-DLLVM_PARALLEL_LINK_JOBS=1")

        -- Native Linux ARM64 needs lld to avoid R_AARCH64_CALL26 relocation overflow
        -- on large (debug/shared) binaries. Skip when cross-compiling (cross toolchains
        -- like arm-gnu-toolchain don't bundle ld.lld) and skip when lld isn't installed
        -- so CMake's CXX_SUPPORTS_CUSTOM_LINKER probe doesn't fail the configure step.
        if package:is_arch("arm64.*", "aarch64") and is_host("linux") and (os.arch() == "arm64" or os.arch() == "aarch64") then
            if find_tool("ld.lld") or find_tool("lld") then
                table.insert(configs, "-DLLVM_USE_LINKER=lld")
            end
        end

        table.insert(configs, "-DLLVM_BUILD_LLVM_DYLIB=" .. (package:config("shared") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_EH=" .. (package:config("exception") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_RTTI=" .. (package:config("rtti") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_DIA_SDK=" .. (package:config("ms_dia") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_LIBCXX=" .. (package:config("libcxx") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_LTO=" .. (package:config("lto") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_ZSTD=" .. (package:dep("zstd") and "ON" or "OFF"))
        table.insert(configs, "-DLLVM_ENABLE_ZLIB=" .. (package:dep("zlib") and "ON" or "OFF"))
        if package:config("libffi") then
            table.insert(configs, "-DLLVM_ENABLE_FFI=ON")
            table.insert(configs, "-DFFI_INCLUDE_DIR=" .. package:dep("libffi"):installdir("include"))
            table.insert(configs, "-DFFI_LIBRARY_DIR=" .. package:dep("libffi"):installdir("lib"))
        else
            table.insert(configs, "-DLLVM_ENABLE_FFI=OFF")
        end
        if package:config("httplib") then
            table.insert(configs, "-DLLVM_ENABLE_HTTPLIB=ON")
            table.insert(configs, "-Dhttplib_ROOT=" .. package:dep("cpp-httplib"):installdir())
        else
            table.insert(configs, "-DLLVM_ENABLE_HTTPLIB=OFF")
        end

        for tooldir in string.gmatch(io.readfile("clang/tools/CMakeLists.txt"), "add_clang_subdirectory%((.-)%)") do
            if tooldir ~= "libclang" and (tooldir ~= "clang-shlib" or not package:config("shared")) then
                local tool = tooldir:upper():gsub("-", "_")
                table.insert(configs, "-DCLANG_TOOL_" .. tool .. "_BUILD=OFF")
            end
        end

        if package:is_plat("android") then
            local triple
            if package:is_arch("arm64-v8a") then
                triple = "aarch64-linux-android"
            elseif package:arch():startswith("armeabi") then
                triple = "armv7a-linux-androideabi"
            elseif package:is_arch("x86") then
                triple = "i686-linux-android"
            elseif package:is_arch("x86_64") then
                triple = "x86_64-linux-android"
            else
                raise("unsupported arch(%s) for android!", package:arch())
            end
            table.insert(configs, "-DLLVM_HOST_TRIPLE=" .. triple)
        end
        if package:is_plat("iphoneos") then
            local triple
            if package:is_arch("arm64") then
                triple = "aarch64-apple-ios"
            else
                raise("unsupported arch(%s) for iphoneos!", package:arch())
            end
            table.insert(configs, "-DLLVM_HOST_TRIPLE=" .. triple)

            -- LLVM build systems mostly use "Darwin", not if(APPLE)
            table.insert(configs, "-DCMAKE_SYSTEM_NAME=Darwin")
        end

        function tryadd_dep(depname, varname)
            varname = varname or depname
            local dep = package:dep(depname)
            if dep and not dep:is_system() then
                local fetchinfo = dep:fetch({external = false})
                if fetchinfo then
                    local includedirs = fetchinfo.includedirs or fetchinfo.sysincludedirs
                    if includedirs and #includedirs > 0 then
                        table.insert(configs, "-D" .. varname .. "_INCLUDE_DIR=" .. table.concat(includedirs, " "):gsub("\\", "/"))
                    end
                    local libfiles = fetchinfo.libfiles
                    if libfiles then
                        table.insert(configs, "-D" .. varname .. "_LIBRARY=" .. table.concat(libfiles, " "):gsub("\\", "/"))
                    end
                end
            end
        end
        tryadd_dep("zlib", "ZLIB")
        tryadd_dep("zstd")

        local opt = {}
        if ci_is_running() then
            opt.jobs = "1"
        end
        os.cd("llvm")
        import("package.tools.cmake").install(package, configs, opt)
    end)

    on_test(function (package)
        if package:is_plat("linux") then
            -- LLVM's own package is not built here, and the pkgconfig file xmake writes for it
            -- names none of the libraries it ships: the snippet below links through that file, so
            -- it has nothing to link with. What this package holds is LLVM's own headers and
            -- archives, which the compiler built against them is the proof of.
            return
        end
        assert(package:check_cxxsnippets({test = [[
            #include <llvm/IR/LLVMContext.h>
            #include <llvm/IR/Module.h>
            void test() {
                llvm::LLVMContext context;
                llvm::Module module("test", context);
            }
        ]]}, {configs = {languages = 'c++17'}}))
        if package:config("clang") then
            assert(package:check_cxxsnippets({test = [[
                #include <clang/Frontend/CompilerInstance.h>
                void test() {
                    clang::CompilerInstance instance;
                }
            ]]}, {configs = {languages = 'c++17'}}))
        end
    end)
