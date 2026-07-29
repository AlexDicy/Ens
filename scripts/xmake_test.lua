-- compile each tests/*.ens with the ens compiler and verify the exit code, the standard output
-- and the standard error against the directives in the test source header:
--     // @exit 12
--     // @stdout Hello!
--     // @stdout-contains Hello
--     // @stderr panic: boom
--     // @stderr-contains panic:
-- @stdout and @stderr give one exact line each and accumulate: every directive of the kind,
-- joined with newlines, must equal the whole stream (trailing newlines trimmed). @stdout-contains
-- and @stderr-contains each name a substring the stream must contain. Both streams are captured
-- separately, so what the runtime reports on stderr - a panic, an unhandled exception - is
-- asserted with the @stderr directives and never appears in stdout.
-- use @expect-error instead to assert the compiler reports a specific diagnostic.
--     // @expect-error Undefined function 'testFunction'
-- a folder test's main.ens may use @ens-test (optionally with extra arguments) to run
-- `ens test <folder> ...` instead of compile+run, asserting on its two streams the same way.
-- the token {dir} in the extra arguments expands to the folder's absolute path.
--     // @ens-test --filter needle
-- tests run in parallel; set ENS_TEST_JOBS to override the worker count (default: cpu count).
-- pass test names to run a subset, e.g. `xmake test arc_basic class_constructor`; runs all if omitted.
task("test")
    set_menu({
        usage = "xmake test [tests...]",
        description = "Run all Ens tests in the tests/ folder",
        options = {
            {nil, "tests", "vs", nil, "Names of specific tests to run (runs all if omitted)"}
        }
    })
    on_run(function()
        import("core.project.config")
        import("core.base.option")
        import("core.base.global")
        import("core.tool.toolchain")
        import("async.runjobs")
        config.load()
        local mode = config.get("mode") or "release"
        local plat = config.get("plat") or os.host()
        local arch = config.get("arch") or os.arch()
        local ens_exe = path.join(os.projectdir(), "build", plat, arch, mode, "ens" .. (is_host("windows") and ".exe" or ""))
        if not os.isfile(ens_exe) then
            print("Building ens compiler...")
            os.exec("xmake build ens")
        end
        if not os.isfile(ens_exe) then
            os.raise("Could not locate ens.exe at " .. ens_exe)
        end

        local tests_dir = path.join(os.projectdir(), "tests")
        local out_dir   = path.join(os.projectdir(), "build", "tests")
        os.mkdir(out_dir)

        -- optional subset: only run the tests named on the command line.
        -- names are matched case-sensitively; a trailing ".ens" is accepted and ignored.
        local wanted = nil
        local requested = option.get("tests")
        if requested and #requested > 0 then
            wanted = {}
            for _, t in ipairs(requested) do
                wanted[(t:gsub("%.ens$", ""))] = false
            end
        end
        local function want(name)
            if wanted == nil then return true end
            if wanted[name] == nil then return false end
            wanted[name] = true
            return true
        end

        -- list of tests:
        --   * every tests/*.ens file (single-file mode)
        --   * every tests/<dir>/main.ens (folder mode)
        --   * every tests/<dir>/src/main.ens (package mode: the folder holds an ens.package)
        local jobs = {}
        for _, ens_file in ipairs(os.files(path.join(tests_dir, "*.ens"))) do
            local name = path.basename(ens_file)
            if want(name) then
                table.insert(jobs, {
                    name = name,
                    ens_file = ens_file,
                    source = ens_file,
                })
            end
        end
        for _, sub in ipairs(os.dirs(path.join(tests_dir, "*"))) do
            local main_ens = path.join(sub, "main.ens")
            local src_main_ens = path.join(sub, "src", "main.ens")
            local name = path.basename(sub)
            if os.isfile(main_ens) and want(name) then
                table.insert(jobs, {
                    name = name,
                    ens_file = main_ens,
                    source = sub,
                })
            elseif os.isfile(src_main_ens) and want(name) then
                table.insert(jobs, {
                    name = name,
                    ens_file = src_main_ens,
                    source = path.join(sub, "src"),
                })
            end
        end

        -- the self-hosted front end's own tests run as one `ens test <package>` job.
        if want("selfhost_frontend") then
            table.insert(jobs, {
                name = "selfhost_frontend",
                source = path.join(os.projectdir(), "selfhost", "frontend"),
                ens_test_args = {},
            })
        end

        -- the syntax.grammar to Ens code generator's tests run the same way.
        if want("selfhost_syntaxgen") then
            table.insert(jobs, {
                name = "selfhost_syntaxgen",
                source = path.join(os.projectdir(), "selfhost", "syntaxgen"),
                ens_test_args = {},
            })
        end

        -- the self-hosted semantic layer's tests run the same way.
        if want("selfhost_sema") then
            table.insert(jobs, {
                name = "selfhost_sema",
                source = path.join(os.projectdir(), "selfhost", "sema"),
                ens_test_args = {},
            })
        end

        -- the corpus round-trip harness parses every .ens file in the real source trees and
        -- asserts the front end is lossless and clean over all of them.
        if want("corpus_roundtrip") then
            table.insert(jobs, {
                name = "corpus_roundtrip",
                corpus = true,
            })
        end

        -- the semantic differential harness runs the self-hosted sema pipeline over every
        -- program unit and gates the build in both directions: accepted units must be clean,
        -- and every @expect-error unit must be rejected unless a seed file tags the diagnostic
        -- as belonging to a later phase with '// @expect-error-at <phase>' (a tag that sema
        -- outgrows fails the run as stale, so the exemption list cannot rot).
        if want("semacheck") then
            table.insert(jobs, {
                name = "semacheck",
                semacheck = true,
            })
        end

        -- the driver's command-line surface: command spellings, retired flags, artifact
        -- naming, libraries, and the hidden cst tools.
        if want("cli_core") then
            table.insert(jobs, {
                name = "cli_core",
                cli_core = true,
            })
        end

        -- the workspace-root and run behaviors: member builds in dependency order, test-all,
        -- application selection, argument passthrough, and exit-code forwarding.
        if want("cli_workspace") then
            table.insert(jobs, {
                name = "cli_workspace",
                cli_workspace = true,
            })
        end

        -- the override subcommands: an add/remove/list roundtrip against a scratch workspace,
        -- name-mismatch validation, and byte-exact edits of ens.overrides.
        if want("cli_override") then
            table.insert(jobs, {
                name = "cli_override",
                cli_override = true,
            })
        end

        -- git-sourced dependencies end to end: tag resolution (verbatim, v-prefix, ambiguity),
        -- fetching into the content store, ens.lock creation, no-network reuse, minimal
        -- updates, --offline and --locked, MVS across transitive requirements, and the
        -- conflict and rejection errors. Everything runs against scratch git repos and a
        -- scratch ENS_CACHE, so no network or real cache is ever touched.
        if want("cli_git") then
            table.insert(jobs, {
                name = "cli_git",
                cli_git = true,
            })
        end

        -- native artifact fetching over file:// URLs: the happy path into the link, cache
        -- reuse under --offline, hash-mismatch rejection, and the artifact lines in ens.lock.
        if want("cli_artifact") then
            table.insert(jobs, {
                name = "cli_artifact",
                cli_artifact = true,
            })
        end

        -- the code-generation differential harness: build the spike (an Ens program that emits an
        -- object file through the ens.llvm binding) and the harness, then drive the spike from
        -- emission through linking and execution and enforce the skip-list anti-rot rule.
        if want("codegencheck") then
            table.insert(jobs, {
                name = "codegencheck",
                codegencheck = true,
            })
        end

        -- the ens.llvm binding's own unit tests: build small LLVM modules through the binding and
        -- assert the LLVM verifier accepts them. They link the native LLVM-C library, so the job
        -- points the linker and loader at the local LLVM package the same way codegencheck does.
        if want("selfhost_llvm") then
            table.insert(jobs, {
                name = "selfhost_llvm",
                llvm_tests = true,
                source = path.join(os.projectdir(), "selfhost", "llvm"),
            })
        end

        -- the self-hosted code generator's unit tests: EIR construction, golden dumps, the
        -- structural verifier, and lowering over analyzed programs. The package depends on the
        -- native ens.llvm binding, so the job carries the same LLVM plumbing.
        if want("selfhost_codegen") then
            table.insert(jobs, {
                name = "selfhost_codegen",
                llvm_tests = true,
                source = path.join(os.projectdir(), "selfhost", "codegen"),
            })
        end

        -- surface any requested names that matched no test, so typos don't pass silently.
        if wanted ~= nil then
            local unknown = {}
            for name, matched in pairs(wanted) do
                if not matched then table.insert(unknown, name) end
            end
            if #unknown > 0 then
                table.sort(unknown)
                os.raise("unknown test(s): %s", table.concat(unknown, ", "))
            end
        end

        -- run a program with stdout and stderr merged through ONE opened handle, so the two
        -- streams append in real order; redirecting both to the same path would open two
        -- handles with independent write cursors that clobber each other at equal offsets.
        -- `opt` may add os.execv options such as curdir or stdin.
        local function execMerged(program, argv, logpath, opt)
            local logfile = io.open(logpath, "w")
            local options = {try = true, stdout = logfile, stderr = logfile}
            for key, value in pairs(opt or {}) do
                options[key] = value
            end
            local rc = os.execv(program, argv, options)
            logfile:close()
            return rc
        end

        -- run a program with each stream captured into its own file, so stdout and stderr are
        -- asserted independently. `opt` may add os.execv options such as curdir or stdin.
        local function execSplit(program, argv, outpath, errpath, opt)
            local outfile = io.open(outpath, "w")
            local errfile = io.open(errpath, "w")
            local options = {try = true, stdout = outfile, stderr = errfile}
            for key, value in pairs(opt or {}) do
                options[key] = value
            end
            local rc = os.execv(program, argv, options)
            outfile:close()
            errfile:close()
            return rc
        end

        -- a captured stream with its trailing newlines trimmed, the form the directives compare.
        local function captured(logpath)
            return ((io.readfile(logpath) or ""):gsub("[\r\n]+$", ""))
        end

        -- the corpus round-trip harness: build the driver exe fresh from its own workspace (it
        -- imports the front end as the @ens.frontend package), enumerate every .ens file and
        -- every ens.package/ens.overrides manifest in the real source trees, and run the
        -- driver over the list.
        local function run_corpus(job)
            local name = job.name
            local corpus_dir  = path.join(os.projectdir(), "build", "corpus")
            local exe_file    = path.join(corpus_dir, "corpus.exe")
            local manifest    = path.join(corpus_dir, "manifest.txt")
            local log         = path.join(out_dir, name .. ".log")
            local corpus_src   = path.join(os.projectdir(), "selfhost", "corpus")

            if not os.isdir(corpus_dir) then
                os.mkdir(corpus_dir)
            end
            os.tryrm(exe_file)
            local compile_rc = execMerged(ens_exe, {"build", corpus_src, "--output", exe_file}, log)
            if not os.isfile(exe_file) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s",
                        name, tostring(compile_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end

            -- enumerate the corpus: every source file plus every manifest file.
            local files = {}
            local seen = {}
            local function add(f)
                local normalized = (f:gsub("\\", "/"))
                if not seen[normalized] then
                    seen[normalized] = true
                    table.insert(files, normalized)
                end
            end
            for _, root in ipairs({"selfhost", "libs", "tests"}) do
                for _, pattern in ipairs({"**.ens", "ens.package", "**/ens.package",
                                          "ens.overrides", "**/ens.overrides"}) do
                    for _, f in ipairs(os.files(path.join(os.projectdir(), root, pattern))) do
                        add(f)
                    end
                end
            end
            table.sort(files)
            io.writefile(manifest, table.concat(files, "\n") .. "\n")

            local run_rc = execMerged(exe_file, {manifest}, log)
            local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
            if run_rc == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("harness exit %s", tostring(run_rc)),
                full = string.format("%s:\n%s", name, out)}
        end

        -- the semantic differential harness: build the driver exe fresh from its own workspace
        -- (it imports the front end and the sema layer as packages), enumerate every program
        -- unit into a manifest, and run the driver over it. Units are the single-file tests,
        -- the folder tests, and the selfhost library packages; libs/std is covered transitively
        -- by every unit that imports @std.
        local function run_semacheck(job)
            local name = job.name
            local check_dir = path.join(os.projectdir(), "build", "semacheck")
            local exe_file  = path.join(check_dir, "semacheck.exe")
            local manifest  = path.join(check_dir, "manifest.txt")
            local log       = path.join(out_dir, name .. ".log")
            local check_src = path.join(os.projectdir(), "selfhost", "semacheck")

            if not os.isdir(check_dir) then
                os.mkdir(check_dir)
            end
            os.tryrm(exe_file)
            local compile_rc = execMerged(ens_exe, {"build", check_src, "--output", exe_file}, log)
            if not os.isfile(exe_file) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s",
                        name, tostring(compile_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end

            -- enumerate the program units.
            local function slashed(p) return (p:gsub("\\", "/")) end
            local lines = {"stdlib " .. slashed(path.join(os.projectdir(), "libs"))}
            local function add_unit(label, source, seeds, entry)
                table.insert(lines, "unit " .. label)
                table.insert(lines, "source " .. slashed(source))
                if entry then
                    table.insert(lines, "entry " .. entry)
                end
                for _, seed in ipairs(seeds) do
                    table.insert(lines, "seed " .. seed)
                end
            end
            -- a single .ens file is compiled as a single-file program: the file itself is the
            -- program's main module regardless of its name, matching the driver.
            local singles = os.files(path.join(tests_dir, "*.ens"))
            table.sort(singles)
            for _, f in ipairs(singles) do
                add_unit("tests/" .. path.filename(f), tests_dir, {}, path.filename(f))
            end
            local folders = os.dirs(path.join(tests_dir, "*"))
            table.sort(folders)
            for _, sub in ipairs(folders) do
                if os.isfile(path.join(sub, "main.ens")) then
                    add_unit("tests/" .. path.basename(sub), sub, {"main.ens"})
                elseif os.isfile(path.join(sub, "src", "main.ens")) then
                    add_unit("tests/" .. path.basename(sub), path.join(sub, "src"), {"main.ens"})
                end
            end
            for _, pkg in ipairs({"corpus", "frontend", "sema", "semacheck", "syntaxgen",
                              "llvm", "codegen", "codegencheck"}) do
                local src = path.join(os.projectdir(), "selfhost", pkg, "src")
                local seeds = {}
                for _, f in ipairs(os.files(path.join(src, "**.ens"))) do
                    table.insert(seeds, slashed(path.relative(f, src)))
                end
                table.sort(seeds)
                add_unit("selfhost/" .. pkg, src, seeds)
            end
            io.writefile(manifest, table.concat(lines, "\n") .. "\n")

            local run_rc = execMerged(exe_file, {manifest}, log)
            local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
            if run_rc == 0 then
                return {name = name, ok = true, note = out:match("expected%-reject caught [^\r\n]+")}
            end
            return {name = name, ok = false,
                short = string.format("harness exit %s", tostring(run_rc)),
                full = string.format("%s:\n%s", name, out)}
        end

        -- the code-generation differential harness: build the harness and the spike, drive the
        -- spike from object emission through linking and execution, and enforce the skip-list
        -- anti-rot rule over the runnable fixtures. The spike links the ens.llvm native binding,
        -- so the build has to point the linker at the local LLVM library and the run has to make
        -- the shared library loadable. That plumbing differs by host and is set up here.
        local function run_codegencheck(job)
            local name = job.name
            local on_windows = is_host("windows")
            local exe_suffix = on_windows and ".exe" or ""
            local check_dir = path.join(os.projectdir(), "build", "codegencheck")
            local harness_exe = path.join(check_dir, "codegencheck" .. exe_suffix)
            local spike_exe = path.join(check_dir, "spike" .. exe_suffix)
            local manifest = path.join(check_dir, "manifest.txt")
            local scratch = path.join(check_dir, "scratch")
            local log = path.join(out_dir, name .. ".log")
            local check_src = path.join(os.projectdir(), "selfhost", "codegencheck")
            local spike_src = path.join(check_src, "spike")
            local skiplist = path.join(check_src, "skiplist.txt")

            os.tryrm(check_dir)
            os.mkdir(check_dir)
            os.mkdir(scratch)

            -- find the local LLVM library the compiler builds against: on Windows the dll plus its
            -- import library, on Linux/macOS the shared object. A machine without it cannot build
            -- the compiler either, so a clear hard failure is the right outcome.
            local packages = path.join(global.directory(), "packages", "l")
            local llvm_lib, llvm_bin, looked
            if on_windows then
                looked = "bin/LLVM-C.dll with lib/LLVM-C.lib"
                for _, dll in ipairs(os.files(path.join(packages, "*", "*", "*", "bin",
                        "LLVM-C.dll"))) do
                    local bindir = path.directory(dll)
                    local libdir = path.join(path.directory(bindir), "lib")
                    if os.isfile(path.join(libdir, "LLVM-C.lib")) then
                        llvm_bin = bindir
                        llvm_lib = libdir
                        break
                    end
                end
            else
                local pattern = is_host("macosx") and "libLLVM*.dylib" or "libLLVM*.so*"
                looked = "lib/" .. pattern
                local matches = os.files(path.join(packages, "*", "*", "*", "lib", pattern))
                table.sort(matches)
                if #matches > 0 then
                    llvm_lib = path.directory(matches[1])
                end
            end
            if not llvm_lib then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: could not find %s under %s; install the LLVM "
                        .. "package the compiler builds against", name, looked, packages)}
            end

            -- build-time linker search paths and run-time library paths. On Windows lld-link reads
            -- LIB (and finds nothing else once LIB is set, so the MSVC and SDK folders must be
            -- added too); on Linux/macOS the ens linker turns LIBRARY_PATH into -L and -rpath, and
            -- the loader honors LD_LIBRARY_PATH / DYLD_LIBRARY_PATH.
            local env = os.getenvs()
            if on_windows then
                local msvc = toolchain.load("msvc", {plat = plat, arch = arch})
                local msvc_lib = ""
                if msvc then
                    local envs = msvc:runenvs()
                    if envs and envs.LIB then msvc_lib = envs.LIB end
                end
                env.LIB = msvc_lib .. ";" .. llvm_lib
                env.PATH = llvm_bin .. ";" .. (env.PATH or "")
            else
                local function prepend(current, dir)
                    if current and #current > 0 then return dir .. ":" .. current end
                    return dir
                end
                env.LIBRARY_PATH = prepend(env.LIBRARY_PATH, llvm_lib)
                if is_host("macosx") then
                    env.DYLD_LIBRARY_PATH = prepend(env.DYLD_LIBRARY_PATH, llvm_lib)
                else
                    env.LD_LIBRARY_PATH = prepend(env.LD_LIBRARY_PATH, llvm_lib)
                end
            end

            -- the harness itself links the native LLVM binding through ens.codegen, so its
            -- build needs the same linker environment as the spike.
            local harness_rc = execMerged(ens_exe,
                {"build", check_src, "--output", harness_exe}, log, {envs = env})
            if not os.isfile(harness_exe) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s", name,
                        tostring(harness_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end
            local spike_rc = execMerged(ens_exe, {"build", spike_src, "--output", spike_exe},
                log, {envs = env})
            if not os.isfile(spike_exe) then
                return {name = name, ok = false, short = "spike build failed",
                    full = string.format("%s: spike build failed (exit %s)\n%s", name,
                        tostring(spike_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end
            -- the Windows loader searches the executable's own folder first; on Linux/macOS the
            -- rpath the linker wrote and the library-path environment cover it.
            if on_windows then
                os.cp(path.join(llvm_bin, "LLVM-C.dll"), path.join(check_dir, "LLVM-C.dll"))
            end

            -- list every candidate fixture; the harness reads each for its directives. A folder
            -- fixture also lists its '*_test.ens' files relative to its source folder, because
            -- the standard library has no directory enumeration yet; the harness narrows them by
            -- the fixture's own @ens-test arguments.
            local lines = {
                "platform " .. (on_windows and "windows" or "posix"),
                "ens " .. ens_exe,
                "spike " .. spike_exe,
                "stdlib " .. (path.join(os.projectdir(), "libs"):gsub("\\", "/")),
                "scratch " .. scratch,
                "skiplist " .. skiplist,
            }
            local singles = os.files(path.join(tests_dir, "*.ens"))
            table.sort(singles)
            for _, f in ipairs(singles) do
                table.insert(lines, "fixture tests/" .. path.filename(f) .. " " .. f)
            end
            local folders = os.dirs(path.join(tests_dir, "*"))
            table.sort(folders)
            for _, sub in ipairs(folders) do
                local main_ens = path.join(sub, "main.ens")
                local src_main_ens = path.join(sub, "src", "main.ens")
                local directive = nil
                if os.isfile(main_ens) then
                    directive = main_ens
                elseif os.isfile(src_main_ens) then
                    directive = src_main_ens
                end
                if directive then
                    local label = "tests/" .. path.basename(sub)
                    table.insert(lines, "fixture " .. label .. " " .. directive)
                    local source_root = path.directory(directive)
                    local test_files = {}
                    for _, tf in ipairs(os.files(path.join(source_root, "**_test.ens"))) do
                        table.insert(test_files, (path.relative(tf, source_root):gsub("\\", "/")))
                    end
                    table.sort(test_files)
                    for _, tf in ipairs(test_files) do
                        table.insert(lines, "testfile " .. label .. " " .. tf)
                    end
                end
            end
            io.writefile(manifest, table.concat(lines, "\n") .. "\n")

            -- the harness runs the spike as a child, so it needs the same library-path
            -- environment for the shared library to load. The fixtures it runs inherit the
            -- same controlled variables the reference runner sets, so the getenv-based ffi
            -- fixture stays hermetic here too.
            env.ENS_TEST_FROMCSTRING_PRESENT = "hermetic"
            env.ENS_TEST_FROMCSTRING_ABSENT = nil
            local run_rc = execMerged(harness_exe, {manifest}, log, {envs = env})
            local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
            if run_rc == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("harness exit %s", tostring(run_rc)),
                full = string.format("%s:\n%s", name, out)}
        end

        -- the ens.llvm binding's unit tests. They link the native LLVM-C library, so the build
        -- needs the LLVM library on the linker path and the run needs it loadable, set up the same
        -- way as the codegen harness.
        local function run_llvm_tests(job)
            local name = job.name
            local on_windows = is_host("windows")
            local log = path.join(out_dir, name .. ".log")
            local packages = path.join(global.directory(), "packages", "l")
            local llvm_lib, llvm_bin, looked
            if on_windows then
                looked = "bin/LLVM-C.dll with lib/LLVM-C.lib"
                for _, dll in ipairs(os.files(path.join(packages, "*", "*", "*", "bin",
                        "LLVM-C.dll"))) do
                    local bindir = path.directory(dll)
                    local libdir = path.join(path.directory(bindir), "lib")
                    if os.isfile(path.join(libdir, "LLVM-C.lib")) then
                        llvm_bin = bindir
                        llvm_lib = libdir
                        break
                    end
                end
            else
                local pattern = is_host("macosx") and "libLLVM*.dylib" or "libLLVM*.so*"
                looked = "lib/" .. pattern
                local matches = os.files(path.join(packages, "*", "*", "*", "lib", pattern))
                table.sort(matches)
                if #matches > 0 then
                    llvm_lib = path.directory(matches[1])
                end
            end
            if not llvm_lib then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: could not find %s under %s; install the LLVM "
                        .. "package the compiler builds against", name, looked, packages)}
            end
            local env = os.getenvs()
            if on_windows then
                local msvc = toolchain.load("msvc", {plat = plat, arch = arch})
                local msvc_lib = ""
                if msvc then
                    local envs = msvc:runenvs()
                    if envs and envs.LIB then msvc_lib = envs.LIB end
                end
                env.LIB = msvc_lib .. ";" .. llvm_lib
                env.PATH = llvm_bin .. ";" .. (env.PATH or "")
            else
                local function prepend(current, dir)
                    if current and #current > 0 then return dir .. ":" .. current end
                    return dir
                end
                env.LIBRARY_PATH = prepend(env.LIBRARY_PATH, llvm_lib)
                if is_host("macosx") then
                    env.DYLD_LIBRARY_PATH = prepend(env.DYLD_LIBRARY_PATH, llvm_lib)
                else
                    env.LD_LIBRARY_PATH = prepend(env.LD_LIBRARY_PATH, llvm_lib)
                end
            end
            local run_rc = execMerged(ens_exe, {"test", job.source}, log, {envs = env})
            local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
            if run_rc == 0 then
                return {name = name, ok = true, note = out:match("(%d+/%d+ tests passed)")}
            end
            return {name = name, ok = false,
                short = string.format("ens test exit %s", tostring(run_rc)),
                full = string.format("%s:\n%s", name, out)}
        end

        -- the driver's command-line surface, exercised end to end against real fixtures.
        local function run_cli_core(job)
            local name = job.name
            local cli_dir = path.join(os.projectdir(), "build", "cli", "core")
            os.tryrm(cli_dir)
            os.mkdir(cli_dir)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(ens_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local function run_program(exe, expected_rc, expected_stdout)
                if not os.isfile(exe) then
                    table.insert(failures, string.format("expected executable %s", exe))
                    return
                end
                local rc = execMerged(exe, {}, log)
                local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            local hello = path.join(tests_dir, "hello.ens")

            -- version and help
            run({"version"}, nil, 0, "ens 0.1")
            run({"--version"}, nil, 0, "ens 0.1")
            run({}, nil, 0, "Usage: ens <command>")
            run({"-h"}, nil, 0, "Usage: ens <command>")
            run({"help", "build"}, nil, 0, "--output")
            local _, help_out = run({"help"}, nil, 0, "build")
            if help_out:find("cst-dump", 1, true) then
                table.insert(failures, "ens help: the hidden cst tools leaked into the help")
            end

            -- retired spellings and usage errors
            run({"--source", hello}, nil, 2, "retired")
            run({"--cst-dump"}, nil, 2, "ens cst-dump")
            run({"frobnicate"}, nil, 2, "Unknown command")
            run({"build", "--output"}, nil, 2, "needs a value")
            run({"build", hello, "extra"}, nil, 2, "Unexpected argument")
            run({"build", path.join(tests_dir, "no_such_place")}, nil, 2, "does not exist")
            run({"test", "--source", tests_dir}, nil, 2, "retired")
            run({"build"}, {curdir = cli_dir}, 2, "No ens.package manifest")

            -- build a single file: explicit output, then the default name in the working folder
            local hello_exe = path.join(cli_dir, "hello_out.exe")
            run({"build", hello, "--output", hello_exe}, nil, 0, "Compiled successfully")
            run_program(hello_exe, 0, "Hello, world!")
            run({"build", hello}, {curdir = cli_dir}, 0, "Compiled successfully")
            run_program(path.join(cli_dir, "hello.exe"), 0, "Hello, world!")

            -- build a package folder: the executable is named after the package
            run({"build", path.join(tests_dir, "pkg_import_main")}, {curdir = cli_dir},
                0, "Compiled successfully")
            run_program(path.join(cli_dir, "main.exe"), 0, "Hello, Ada! [acme.tools]")

            -- a package without main() is a library: it validates fully, keeps no artifact,
            -- and refuses an explicit output
            run({"build", path.join(tests_dir, "pkg_import_dep")}, {curdir = cli_dir},
                0, "library")
            if os.isfile(path.join(cli_dir, "tools.exe")) then
                table.insert(failures, "building a library left an executable behind")
            end
            run({"build", path.join(tests_dir, "pkg_import_dep"), "--output",
                path.join(cli_dir, "tools.exe")}, nil, 1, "does not define main()")

            -- check: no artifacts, plain success and failure
            run({"check", hello}, nil, 0, "No problems found")
            run({"check", path.join(tests_dir, "pkg_import_dep")}, nil, 0, "No problems found")
            run({"check", path.join(tests_dir, "undefined_function.ens")}, nil, 1,
                "Undefined function")

            -- hidden cst tools: a path argument and stdin
            run({"cst-dump", hello}, nil, 0, "Typed outline")
            run({"cst-analyze", hello}, nil, 0)
            run({"cst-dump"}, {stdin = hello}, 0, "Typed outline")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- workspace-root builds and the run command, exercised against the cli_workspace
        -- fixtures.
        local function run_cli_workspace(job)
            local name = job.name
            local cli_dir = path.join(os.projectdir(), "build", "cli", "workspace")
            os.tryrm(cli_dir)
            os.mkdir(cli_dir)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(ens_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local workspace = path.join(tests_dir, "cli_workspace")
            local two_apps = path.join(tests_dir, "cli_workspace_two_apps")

            -- build every member in dependency order: the library goes first even though the
            -- manifest lists the application first, and only the application leaves an artifact
            local _, build_out = run({"build", workspace}, {curdir = cli_dir}, 0,
                "[1/2] demo.lib: library ok", "[2/2] demo.app: built app.exe")
            if not os.isfile(path.join(cli_dir, "app.exe")) then
                table.insert(failures, "the workspace build did not produce app.exe")
            end
            run({"build", workspace, "--output", path.join(cli_dir, "x.exe")}, nil, 2,
                "--output")

            -- check and test every member
            run({"check", workspace}, nil, 0, "[1/2] demo.lib: ok", "[2/2] demo.app: ok")
            run({"test", workspace}, nil, 0, "PASS greeting is stable", "1/1 tests passed",
                "demo.app")

            -- run: everything after -- reaches the program, its exit code comes back, and
            -- nothing is left in the working folder
            local run_dir = path.join(cli_dir, "rundir")
            os.mkdir(run_dir)
            run({"run", workspace, "--", "alpha", "beta"}, {curdir = run_dir}, 2,
                "cli workspace greeting", "alpha", "beta")
            run({"run", path.join(two_apps, "one")}, {curdir = run_dir}, 7)
            if #os.files(path.join(run_dir, "*")) ~= 0 then
                table.insert(failures, "ens run left artifacts in the working folder")
            end

            -- run needs exactly one application
            run({"run", two_apps}, nil, 2, "more than one application member", "demo.one",
                "demo.two")
            run({"run", path.join(tests_dir, "pkg_import_dep")}, nil, 2,
                "is not an application")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- the override subcommands, driven against a scratch workspace so the repo tree is
        -- never touched.
        local function run_cli_override(job)
            local name = job.name
            local scratch = path.join(os.projectdir(), "build", "cli", "override")
            os.tryrm(scratch)
            local ws_dir = path.join(scratch, "ws")
            os.mkdir(path.join(ws_dir, "app", "src"))
            os.mkdir(path.join(scratch, "json", "src"))
            os.mkdir(path.join(scratch, "wrong"))
            io.writefile(path.join(ws_dir, "ens.package"),
                'workspace {\n    member "app";\n}\n')
            io.writefile(path.join(ws_dir, "app", "ens.package"),
                'package demo.app {\n    ens "0.1";\n\n    dependency acme.json "1.0";\n}\n')
            io.writefile(path.join(ws_dir, "app", "src", "main.ens"),
                'import @acme.json.parse;\n\nmain() -> int {\n    print(parse.tag());\n'
                .. '    return 0;\n}\n')
            io.writefile(path.join(scratch, "json", "ens.package"),
                'package acme.json {\n    ens "0.1";\n}\n')
            io.writefile(path.join(scratch, "json", "src", "parse.ens"),
                'export tag() -> string {\n    return "json override";\n}\n')
            io.writefile(path.join(scratch, "wrong", "ens.package"),
                'package acme.other {\n    ens "0.1";\n}\n')

            local overrides_file = path.join(ws_dir, "ens.overrides")
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(ens_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local function normalized(text)
                return (text or ""):gsub("\r\n", "\n")
            end

            local function expect_file(label, expected)
                local actual = normalized(io.readfile(overrides_file))
                if actual ~= expected then
                    table.insert(failures, string.format("%s: ens.overrides is %q, expected %q",
                        label, actual, expected))
                end
            end

            local in_ws = {curdir = ws_dir}

            -- without an override, the dependency has no source
            run({"override", "list"}, in_ws, 0, "No overrides")
            run({"build"}, in_ws, 1, "No source for package 'acme.json'")

            -- add creates the file, list validates the target, and the build resolves
            -- through the override (the note names it)
            run({"override", "add", "acme.json", "../json"}, in_ws, 0, "Added the override")
            expect_file("add", 'overrides {\n    override acme.json "../json";\n}\n')
            run({"override", "list"}, in_ws, 0, 'acme.json -> ../json (ok)')
            run({"build"}, in_ws, 0, "Using the override for package 'acme.json'",
                "[1/1] demo.app: built app.exe")
            run({"run", "--"}, in_ws, 0, "json override")

            -- a target that declares a different package is rejected and nothing changes
            run({"override", "add", "acme.json", "../wrong"}, in_ws, 1,
                "declares package 'acme.other' instead")
            run({"override", "list"}, in_ws, 0, 'acme.json -> ../json (ok)')

            -- edits are targeted: comments and other declarations survive byte-exact
            io.writefile(overrides_file, '// local checkouts\noverrides {\n'
                .. '    override acme.json "../wrong";\n'
                .. '    override beta.tools "../missing";\n}\n')
            run({"override", "add", "acme.json", "../json"}, in_ws, 0, "Replaced the override")
            expect_file("replace", '// local checkouts\noverrides {\n'
                .. '    override acme.json "../json";\n'
                .. '    override beta.tools "../missing";\n}\n')
            run({"override", "remove", "acme.json"}, in_ws, 0, "Removed the override")
            expect_file("remove", '// local checkouts\noverrides {\n'
                .. '    override beta.tools "../missing";\n}\n')
            run({"override", "list"}, in_ws, 0, "beta.tools -> ../missing (invalid:")

            -- usage and state errors
            run({"override", "remove", "nope.pkg"}, in_ws, 1, "No override for package")
            run({"override", "add", "not/a/name!", "../json"}, in_ws, 2,
                "not a valid package name")
            run({"override", "add", "acme.json"}, in_ws, 2, "takes a package name and a folder")
            run({"override", "wat"}, in_ws, 2, "Unknown form")
            run({"override", "list"}, {curdir = scratch}, 2, "No ens.package manifest")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- git-sourced dependencies, driven against scratch git repositories over file://
        -- URLs and a scratch ENS_CACHE, so nothing touches the network or the real cache.
        local function run_cli_git(job)
            local name = job.name
            local scratch = path.join(os.projectdir(), "build", "cli", "git")
            os.tryrm(scratch)
            os.tryrm(scratch .. ".away")
            os.mkdir(scratch)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local cache = path.join(scratch, "cache")
            local cache_cold = path.join(scratch, "cache_cold")
            local repos = path.join(scratch, "repos")

            local function envs_with_cache(cache_dir)
                local envs = os.getenvs()
                envs.ENS_CACHE = cache_dir
                return envs
            end

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(ens_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local function run_program(exe, expected_rc, expected_stdout)
                if not os.isfile(exe) then
                    table.insert(failures, string.format("expected executable %s", exe))
                    return
                end
                local rc = execMerged(exe, {}, log)
                local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            -- a scratch repository keeps its metadata in a sibling folder instead of a '.git'
            -- child, so no folder under build/ looks like a VCS root to an IDE. Fetching reads
            -- the metadata folder directly, which is what the file:// URLs point at.
            local function gitdir_of(dir)
                return path.absolute(dir) .. ".gitdir"
            end

            local function git(dir, ...)
                os.iorunv("git", table.join({"--git-dir", gitdir_of(dir), "--work-tree",
                    path.absolute(dir), "-c", "user.name=ens", "-c",
                    "user.email=ens@test", "-c", "commit.gpgsign=false", "-c",
                    "tag.gpgsign=false"}, {...}), {curdir = dir})
            end

            local function make_repo(dir)
                os.mkdir(dir)
                os.iorunv("git", {"init", "--bare", "-q", "-b", "main", gitdir_of(dir)})
            end

            local function commit_all(dir, message)
                git(dir, "add", "-A")
                git(dir, "commit", "-q", "-m", message)
            end

            local function commit_of(dir, tag)
                local out = os.iorunv("git", {"--git-dir", gitdir_of(dir), "rev-parse",
                    tag .. "^{commit}"})
                return (out:gsub("%s+$", ""))
            end

            local function url_of(dir)
                return "file:///" .. (gitdir_of(dir):gsub("\\", "/"))
            end

            local function write_package(dir, package_name, dependencies)
                local lines = {"package " .. package_name .. " {", '    ens "0.1";'}
                if dependencies and #dependencies > 0 then
                    table.insert(lines, "")
                    for _, dep in ipairs(dependencies) do
                        table.insert(lines, "    " .. dep)
                    end
                end
                table.insert(lines, "}")
                io.writefile(path.join(dir, "ens.package"), table.concat(lines, "\n") .. "\n")
            end

            -- the scratch repositories: a plain package with several tagged versions, a
            -- package with a transitive git requirement, a workspace-form tag root, a
            -- submodule user, and a package that will be re-tagged.
            local json_dir = path.join(repos, "json")
            make_repo(json_dir)
            write_package(json_dir, "alex.json")
            io.writefile(path.join(json_dir, "src", "parse.ens"),
                'export tag() -> string {\n    return "json 1.0";\n}\n')
            commit_all(json_dir, "1.0")
            git(json_dir, "tag", "1.0")
            io.writefile(path.join(json_dir, "src", "parse.ens"),
                'export tag() -> string {\n    return "json 1.1";\n}\n')
            commit_all(json_dir, "1.1")
            git(json_dir, "tag", "v1.1")
            io.writefile(path.join(json_dir, "src", "parse.ens"),
                'export tag() -> string {\n    return "json 2.0";\n}\n')
            commit_all(json_dir, "2.0")
            git(json_dir, "tag", "2.0")
            git(json_dir, "tag", "v2.0")
            local url_json = url_of(json_dir)

            local utils_dir = path.join(repos, "utils")
            make_repo(utils_dir)
            write_package(utils_dir, "beta.utils")
            io.writefile(path.join(utils_dir, "src", "helper.ens"),
                'export describe() -> string {\n    return "utils 1.0";\n}\n')
            commit_all(utils_dir, "1.0")
            git(utils_dir, "tag", "1.0")
            io.writefile(path.join(utils_dir, "src", "helper.ens"),
                'export describe() -> string {\n    return "utils 1.1";\n}\n')
            commit_all(utils_dir, "1.1")
            git(utils_dir, "tag", "1.1")
            local url_utils = url_of(utils_dir)

            local tools_dir = path.join(repos, "tools")
            make_repo(tools_dir)
            write_package(tools_dir, "acme.tools",
                {'dependency beta.utils "1.1" from "' .. url_utils .. '";'})
            io.writefile(path.join(tools_dir, "src", "tool.ens"),
                'import @beta.utils.helper;\n\nexport describe() -> string {\n'
                .. '    return "tools(" + helper.describe() + ")";\n}\n')
            commit_all(tools_dir, "1.0")
            git(tools_dir, "tag", "1.0")
            local url_tools = url_of(tools_dir)

            local ws_dir = path.join(repos, "ws")
            make_repo(ws_dir)
            io.writefile(path.join(ws_dir, "ens.package"),
                'workspace {\n    member "core";\n    member "extra";\n}\n')
            write_package(path.join(ws_dir, "core"), "ws.core", {"dependency ws.extra;"})
            io.writefile(path.join(ws_dir, "core", "src", "api.ens"),
                'import @ws.extra.helper;\n\nexport describe() -> string {\n'
                .. '    return "core(" + helper.describe() + ")";\n}\n')
            write_package(path.join(ws_dir, "extra"), "ws.extra")
            io.writefile(path.join(ws_dir, "extra", "src", "helper.ens"),
                'export describe() -> string {\n    return "extra";\n}\n')
            commit_all(ws_dir, "1.0")
            git(ws_dir, "tag", "1.0")
            local url_ws = url_of(ws_dir)

            local sub_dir = path.join(repos, "sub")
            make_repo(sub_dir)
            write_package(sub_dir, "sub.pkg")
            io.writefile(path.join(sub_dir, ".gitmodules"),
                '[submodule "vendored"]\n\tpath = vendored\n\turl = https://example.com/x.git\n')
            io.writefile(path.join(sub_dir, "src", "x.ens"),
                'export x() -> int {\n    return 0;\n}\n')
            commit_all(sub_dir, "1.0")
            git(sub_dir, "tag", "1.0")
            local url_sub = url_of(sub_dir)

            local retag_dir = path.join(repos, "retag")
            make_repo(retag_dir)
            write_package(retag_dir, "rt.pkg")
            io.writefile(path.join(retag_dir, "src", "thing.ens"),
                'export tag() -> string {\n    return "retag 1";\n}\n')
            commit_all(retag_dir, "1.0")
            git(retag_dir, "tag", "1.0")
            local url_retag = url_of(retag_dir)

            -- the main application: three git dependencies, one of which raises another's
            -- version transitively (MVS selects the maximum requirement).
            local app1 = path.join(scratch, "app1")
            local function write_app1(json_version)
                write_package(app1, "demo.gitapp", {
                    'dependency alex.json "' .. json_version .. '" from "' .. url_json .. '";',
                    'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                    'dependency beta.utils "1.0" from "' .. url_utils .. '";',
                })
            end
            write_app1("1.0")
            io.writefile(path.join(app1, "src", "main.ens"),
                'import @alex.json.parse;\nimport @acme.tools.tool;\n\n'
                .. 'main() -> int {\n    print(parse.tag() + " | " + tool.describe());\n'
                .. '    return 0;\n}\n')
            local in_app1 = {curdir = app1, envs = envs_with_cache(cache)}
            local lock_file = path.join(app1, "ens.lock")
            local function lock_text()
                return ((io.readfile(lock_file) or ""):gsub("\r\n", "\n"))
            end

            -- first build: fetches, locks, links, and the program runs against the
            -- transitively raised beta.utils 1.1
            run({"build", "."}, in_app1, 0,
                "Fetched alex.json 1.0 from " .. url_json .. " (tag 1.0).",
                "Fetched acme.tools 1.0",
                "Fetched beta.utils 1.1",
                "Updated ens.lock: locked acme.tools 1.0, locked alex.json 1.0, "
                    .. "locked beta.utils 1.1")
            run_program(path.join(app1, "gitapp.exe"), 0, "json 1.0 | tools(utils 1.1)")

            -- the lock is deterministic, sorted, and complete
            local lock1 = lock_text()
            local expected_head = "lock 1\nroot demo.gitapp\npackage acme.tools 1.0\n"
                .. "source " .. url_tools .. " " .. commit_of(tools_dir, "1.0") .. "\n"
            if lock1:sub(1, #expected_head) ~= expected_head then
                table.insert(failures, string.format("lock head is %q, expected %q",
                    lock1:sub(1, #expected_head), expected_head))
            end
            for _, fragment in ipairs({
                "\nrequire beta.utils 1.1\n",
                "\npackage alex.json 1.0\nsource " .. url_json .. " "
                    .. commit_of(json_dir, "1.0") .. "\ncontent sha256:",
                "\npackage beta.utils 1.1\nsource " .. url_utils .. " "
                    .. commit_of(utils_dir, "1.1") .. "\ncontent sha256:",
            }) do
                if not lock1:find(fragment, 1, true) then
                    table.insert(failures, string.format("ens.lock missing %q\n%s",
                        fragment, lock1))
                end
            end

            -- the fetched tree sits in the content store under its locked hash
            local json_hex = lock1:match("package alex%.json 1%.0\nsource [^\n]*\n"
                .. "content sha256:(%x+)")
            if not json_hex then
                table.insert(failures, "could not find alex.json's content hash in the lock")
            elseif not os.isdir(path.join(cache, "trees", "sha256-" .. json_hex)) then
                table.insert(failures, "the content store has no tree for alex.json's hash")
            end

            -- a locked build needs no network: the repositories are gone, yet the build
            -- reproduces byte-identically from the lock and the content store
            os.mv(repos, repos .. ".away")
            local _, reuse_out = run({"build", "."}, in_app1, 0, "Compiled successfully")
            if reuse_out:find("Fetched", 1, true) then
                table.insert(failures, "a locked, cached build still fetched something")
            end
            if lock_text() ~= lock1 then
                table.insert(failures, "a locked, cached build rewrote ens.lock")
            end
            run({"build", ".", "--offline"}, in_app1, 0, "Compiled successfully")
            run({"build", ".", "--offline"},
                {curdir = app1, envs = envs_with_cache(cache_cold)}, 1,
                "--offline forbids fetching package")
            os.mv(repos .. ".away", repos)

            -- --locked turns a pending lock change into an error and leaves the lock alone
            write_app1("1.1")
            run({"build", ".", "--locked"}, in_app1, 1, "ens.lock is out of date",
                "updated alex.json 1.0 -> 1.1", "--locked forbids")
            if lock_text() ~= lock1 then
                table.insert(failures, "--locked changed ens.lock")
            end

            -- the minimal update fetches the raised version through its v-prefixed tag and
            -- summarizes the change
            run({"build", "."}, in_app1, 0,
                "Fetched alex.json 1.1 from " .. url_json .. " (tag v1.1).",
                "Updated ens.lock: updated alex.json 1.0 -> 1.1")
            run_program(path.join(app1, "gitapp.exe"), 0, "json 1.1 | tools(utils 1.1)")
            if not lock_text():find("package alex.json 1.1", 1, true) then
                table.insert(failures, "ens.lock does not record the updated version")
            end

            -- both '2.0' and 'v2.0' exist, so version 2.0 is ambiguous
            write_app1("2.0")
            run({"build", "."}, in_app1, 1, 'version "2.0" of package \'alex.json\' is '
                .. "ambiguous")

            -- requirements spanning majors are a hard error naming both requirers
            local app2 = path.join(scratch, "app2")
            write_package(app2, "demo.major", {
                'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                'dependency beta.utils "2.0" from "' .. url_utils .. '";',
            })
            io.writefile(path.join(app2, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = app2, envs = envs_with_cache(cache)}, 1,
                "The requirements on package 'beta.utils' span major versions",
                'requires "1.1"', 'requires "2.0"')

            -- every package must agree on a dependency's source URL
            local app3 = path.join(scratch, "app3")
            write_package(app3, "demo.urls", {
                'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                'dependency beta.utils "1.0" from "' .. url_json .. '";',
            })
            io.writefile(path.join(app3, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = app3, envs = envs_with_cache(cache)}, 1,
                "Package 'beta.utils' is required from different git sources",
                "every package must agree on a dependency's source")

            -- the tag root must declare the required package
            local app_name = path.join(scratch, "app_name")
            write_package(app_name, "demo.wrongname", {
                'dependency wrong.name "1.0" from "' .. url_json .. '";',
            })
            io.writefile(path.join(app_name, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = app_name, envs = envs_with_cache(cache)}, 1,
                "declares package 'alex.json', not 'wrong.name'")

            -- a workspace-form tag root resolves the required member, and the members
            -- resolve each other inside the fetched tree; the sibling is never locked
            local app_ws = path.join(scratch, "app_ws")
            write_package(app_ws, "demo.wsapp", {
                'dependency ws.core "1.0" from "' .. url_ws .. '";',
            })
            io.writefile(path.join(app_ws, "src", "main.ens"),
                'import @ws.core.api;\n\nmain() -> int {\n    print(api.describe());\n'
                .. '    return 0;\n}\n')
            run({"build", "."}, {curdir = app_ws, envs = envs_with_cache(cache)}, 0,
                "Fetched ws.core 1.0")
            run_program(path.join(app_ws, "wsapp.exe"), 0, "core(extra)")
            local ws_lock = (io.readfile(path.join(app_ws, "ens.lock")) or "")
            if not ws_lock:find("package ws.core 1.0", 1, true) then
                table.insert(failures, "the workspace-form package was not locked")
            end
            if ws_lock:find("ws.extra", 1, true) then
                table.insert(failures, "a member internal to the fetched workspace leaked "
                    .. "into the lock")
            end

            -- packages using git submodules are rejected
            local app_sub = path.join(scratch, "app_sub")
            write_package(app_sub, "demo.subapp", {
                'dependency sub.pkg "1.0" from "' .. url_sub .. '";',
            })
            io.writefile(path.join(app_sub, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = app_sub, envs = envs_with_cache(cache)}, 1,
                "uses git submodules", "self-contained")

            -- a moved tag is caught: the fetched content no longer matches the lock
            local app_retag = path.join(scratch, "app_retag")
            write_package(app_retag, "demo.retag", {
                'dependency rt.pkg "1.0" from "' .. url_retag .. '";',
            })
            io.writefile(path.join(app_retag, "src", "main.ens"),
                'import @rt.pkg.thing;\n\nmain() -> int {\n    print(thing.tag());\n'
                .. '    return 0;\n}\n')
            local in_retag = {curdir = app_retag, envs = envs_with_cache(cache)}
            run({"build", "."}, in_retag, 0, "Fetched rt.pkg 1.0")
            run_program(path.join(app_retag, "retag.exe"), 0, "retag 1")
            io.writefile(path.join(retag_dir, "src", "thing.ens"),
                'export tag() -> string {\n    return "retag 2";\n}\n')
            commit_all(retag_dir, "moved")
            git(retag_dir, "tag", "-f", "1.0")
            os.tryrm(path.join(cache, "trees"))
            run({"build", "."}, in_retag, 1, "does not match ens.lock",
                "the lock records sha256:", "hashes to sha256:", "may have been moved")

            -- dropping the last git dependency removes the lock
            write_package(app_retag, "demo.retag")
            io.writefile(path.join(app_retag, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, in_retag, 0,
                "Removed ens.lock: the build has no git-sourced packages.")
            if os.isfile(path.join(app_retag, "ens.lock")) then
                table.insert(failures, "ens.lock survived losing its last git dependency")
            end

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- native artifact fetching, driven with file:// URLs and a scratch ENS_CACHE. The
        -- artifact is a valid empty static library, so every platform's linker accepts it.
        local function run_cli_artifact(job)
            local name = job.name
            local scratch = path.join(os.projectdir(), "build", "cli", "artifact")
            os.tryrm(scratch)
            os.mkdir(scratch)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local cache = path.join(scratch, "cache")
            local cache_cold = path.join(scratch, "cache_cold")

            local function envs_with_cache(cache_dir)
                local envs = os.getenvs()
                envs.ENS_CACHE = cache_dir
                return envs
            end

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(ens_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local function run_program(exe, expected_rc, expected_stdout)
                if not os.isfile(exe) then
                    table.insert(failures, string.format("expected executable %s", exe))
                    return
                end
                local rc = execMerged(exe, {}, log)
                local out = (io.readfile(log) or ""):gsub("[\r\n]+$", "")
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            local files = path.join(scratch, "files")
            os.mkdir(files)
            local lib_file = path.join(files, "extras.lib")
            io.writefile(lib_file, "!<arch>\n", {encoding = "binary"})
            local good_hash = "sha256:" .. hash.sha256(lib_file)
            local bad_hash = "sha256:" .. string.rep("0123456789abcdef", 4)
            local url_lib = "file:///" .. (path.absolute(lib_file):gsub("\\", "/"))

            local function artifact_native(native_name, artifact_hash)
                return "    native " .. native_name .. " {\n"
                    .. '        windows artifact "' .. url_lib .. '" hash "'
                    .. artifact_hash .. '";\n'
                    .. '        linux artifact "' .. url_lib .. '" hash "'
                    .. artifact_hash .. '";\n'
                    .. '        macos artifact "' .. url_lib .. '" hash "'
                    .. artifact_hash .. '";\n'
                    .. "    }\n"
            end

            -- the happy path: the artifact is fetched, verified, cached, and linked
            local app1 = path.join(scratch, "app1")
            os.mkdir(path.join(app1, "src"))
            io.writefile(path.join(app1, "ens.package"),
                "package demo.artifactapp {\n" .. '    ens "0.1";\n\n'
                .. artifact_native("extras", good_hash) .. "}\n")
            io.writefile(path.join(app1, "src", "main.ens"),
                'main() -> int {\n    print("artifact linked");\n    return 0;\n}\n')
            local in_app1 = {curdir = app1, envs = envs_with_cache(cache)}
            run({"build", "."}, in_app1, 0, "Compiled successfully")
            run_program(path.join(app1, "artifactapp.exe"), 0, "artifact linked")
            local stored = path.join(cache, "artifacts", good_hash:gsub("^sha256:", ""),
                "extras.lib")
            if not os.isfile(stored) then
                table.insert(failures, "the fetched artifact is not in the content store")
            end

            -- the cached artifact satisfies --offline even with the source file gone
            os.mv(files, files .. ".away")
            run({"build", ".", "--offline"}, in_app1, 0, "Compiled successfully")
            run({"build", ".", "--offline"},
                {curdir = app1, envs = envs_with_cache(cache_cold)}, 1,
                "--offline forbids fetching the native artifact")
            os.mv(files .. ".away", files)

            -- a hash mismatch is rejected, naming both hashes
            local app2 = path.join(scratch, "app2")
            os.mkdir(path.join(app2, "src"))
            io.writefile(path.join(app2, "ens.package"),
                "package demo.badhash {\n" .. '    ens "0.1";\n\n'
                .. artifact_native("extras", bad_hash) .. "}\n")
            io.writefile(path.join(app2, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = app2, envs = envs_with_cache(cache)}, 1,
                "hashes to " .. good_hash, "declares " .. bad_hash, "refusing to use it")

            -- ens.lock records the artifact bindings of the root package and of every
            -- fetched package, flattened per platform
            local function gitdir_of(dir)
                return path.absolute(dir) .. ".gitdir"
            end
            local function git(dir, ...)
                os.iorunv("git", table.join({"--git-dir", gitdir_of(dir), "--work-tree",
                    path.absolute(dir), "-c", "user.name=ens", "-c",
                    "user.email=ens@test", "-c", "commit.gpgsign=false", "-c",
                    "tag.gpgsign=false"}, {...}), {curdir = dir})
            end
            local dep_dir = path.join(scratch, "repos", "dep")
            os.mkdir(dep_dir)
            os.iorunv("git", {"init", "--bare", "-q", "-b", "main", gitdir_of(dep_dir)})
            io.writefile(path.join(dep_dir, "ens.package"),
                "package art.dep {\n" .. '    ens "0.1";\n\n'
                .. artifact_native("depextras", good_hash) .. "}\n")
            io.writefile(path.join(dep_dir, "src", "dep.ens"),
                'export tag() -> string {\n    return "dep with artifact";\n}\n')
            git(dep_dir, "add", "-A")
            git(dep_dir, "commit", "-q", "-m", "1.0")
            git(dep_dir, "tag", "1.0")
            local url_dep = "file:///" .. (gitdir_of(dep_dir):gsub("\\", "/"))

            local app3 = path.join(scratch, "app3")
            os.mkdir(path.join(app3, "src"))
            io.writefile(path.join(app3, "ens.package"),
                "package demo.lockapp {\n" .. '    ens "0.1";\n\n'
                .. '    dependency art.dep "1.0" from "' .. url_dep .. '";\n\n'
                .. artifact_native("extras", good_hash) .. "}\n")
            io.writefile(path.join(app3, "src", "main.ens"),
                'import @art.dep.dep;\n\nmain() -> int {\n    print(dep.tag());\n'
                .. '    return 0;\n}\n')
            run({"build", "."}, {curdir = app3, envs = envs_with_cache(cache)}, 0,
                "Fetched art.dep 1.0", "Updated ens.lock: locked art.dep 1.0")
            run_program(path.join(app3, "lockapp.exe"), 0, "dep with artifact")
            local lock = ((io.readfile(path.join(app3, "ens.lock")) or ""):gsub("\r\n", "\n"))
            local root_lines = "root demo.lockapp\n"
                .. "artifact extras linux " .. url_lib .. " " .. good_hash .. "\n"
                .. "artifact extras macos " .. url_lib .. " " .. good_hash .. "\n"
                .. "artifact extras windows " .. url_lib .. " " .. good_hash .. "\n"
            if not lock:find(root_lines, 1, true) then
                table.insert(failures, string.format(
                    "ens.lock is missing the root artifact lines:\n%s", lock))
            end
            local dep_lines = "artifact depextras linux " .. url_lib .. " " .. good_hash
                .. "\nartifact depextras macos " .. url_lib .. " " .. good_hash
                .. "\nartifact depextras windows " .. url_lib .. " " .. good_hash .. "\n"
            if not (lock:find("package art.dep 1.0\n", 1, true)
                    and lock:find(dep_lines, 1, true)) then
                table.insert(failures, string.format(
                    "ens.lock is missing the fetched package's artifact lines:\n%s", lock))
            end

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- run a single test: compile, optionally run, and compare against the header.
        -- returns { name = ..., ok = bool, short = <fail reason>, full = <detailed report> }.
        local function run_one(job)
            if job.corpus then return run_corpus(job) end
            if job.semacheck then return run_semacheck(job) end
            if job.cli_core then return run_cli_core(job) end
            if job.cli_workspace then return run_cli_workspace(job) end
            if job.cli_override then return run_cli_override(job) end
            if job.cli_git then return run_cli_git(job) end
            if job.cli_artifact then return run_cli_artifact(job) end
            if job.codegencheck then return run_codegencheck(job) end
            if job.llvm_tests then return run_llvm_tests(job) end
            local name = job.name
            local ens_file = job.ens_file
            local exe_file    = path.join(out_dir, name .. ".exe")
            local stdout_file = path.join(out_dir, name .. ".stdout")
            local stderr_file = path.join(out_dir, name .. ".stderr")
            local compile_log = path.join(out_dir, name .. ".compile.log")

            local expected_exit     = 0
            local expected_stdout   = nil   -- list of lines; joined with "\n" for an exact match
            local expected_contains = {}    -- substrings that must each appear in stdout
            local expected_stderr   = nil   -- the same, for the standard error stream
            local expected_stderr_contains = {}
            local expected_error    = nil
            local ens_test_args     = job.ens_test_args   -- @ens-test: run `ens test` on the folder instead
            local content = (ens_file and io.readfile(ens_file)) or ""
            for line in content:gmatch("[^\r\n]+") do
                local exit_str = line:match("^%s*//%s*@exit%s+(%-?%d+)")
                if exit_str then expected_exit = tonumber(exit_str) end
                local contains_str = line:match("^%s*//%s*@stdout%-contains%s+(.*)$")
                if contains_str then table.insert(expected_contains, contains_str) end
                local stdout_str = line:match("^%s*//%s*@stdout%s+(.*)$")
                if stdout_str then
                    if expected_stdout == nil then expected_stdout = {} end
                    table.insert(expected_stdout, stdout_str)
                end
                local err_contains_str = line:match("^%s*//%s*@stderr%-contains%s+(.*)$")
                if err_contains_str then
                    table.insert(expected_stderr_contains, err_contains_str)
                end
                local stderr_str = line:match("^%s*//%s*@stderr%s+(.*)$")
                if stderr_str then
                    if expected_stderr == nil then expected_stderr = {} end
                    table.insert(expected_stderr, stderr_str)
                end
                local error_str = line:match("^%s*//%s*@expect%-error%s+(.*)$")
                if error_str then expected_error = error_str end
                local enstest_str = line:match("^%s*//%s*@ens%-test%s*(.*)$")
                if enstest_str then
                    ens_test_args = {}
                    for token in enstest_str:gmatch("%S+") do
                        table.insert(ens_test_args, token)
                    end
                end
            end

            -- compare a process result against the @exit directive and, per stream, its exact-line
            -- and contains directives.
            local function compareRun(run_rc, actual_stdout, actual_stderr)
                local why = {}
                if run_rc ~= expected_exit then
                    table.insert(why, string.format("exit=%s expected=%s",
                        tostring(run_rc), tostring(expected_exit)))
                end
                local streams = {
                    {label = "stdout", actual = actual_stdout,
                     lines = expected_stdout, contains = expected_contains},
                    {label = "stderr", actual = actual_stderr,
                     lines = expected_stderr, contains = expected_stderr_contains},
                }
                for _, stream in ipairs(streams) do
                    if stream.lines ~= nil then
                        local joined = table.concat(stream.lines, "\n")
                        if stream.actual ~= joined then
                            table.insert(why, string.format("%s=%q expected=%q",
                                stream.label, stream.actual, joined))
                        end
                    end
                    for _, sub in ipairs(stream.contains) do
                        if not stream.actual:find(sub, 1, true) then
                            table.insert(why, string.format("%s missing %q", stream.label, sub))
                        end
                    end
                end
                return why
            end

            -- @ens-test: invoke `ens test` on the folder and assert on its two streams.
            if ens_test_args ~= nil then
                os.tryrm(stdout_file)
                os.tryrm(stderr_file)
                local argv = {"test", job.source}
                for _, a in ipairs(ens_test_args) do
                    table.insert(argv, (a:gsub("{dir}", (job.source:gsub("\\", "/")))))
                end
                local run_rc = execSplit(ens_exe, argv, stdout_file, stderr_file)
                local actual_stdout = captured(stdout_file)
                local actual_stderr = captured(stderr_file)
                local why = compareRun(run_rc, actual_stdout, actual_stderr)
                if #why == 0 then
                    return {name = name, ok = true}
                end
                local short = table.concat(why, "; ")
                return {name = name, ok = false, short = short,
                    full = string.format("%s: %s\nstdout:\n%s\nstderr:\n%s",
                        name, short, actual_stdout, actual_stderr)}
            end

            os.tryrm(exe_file)
            os.tryrm(stdout_file)
            os.tryrm(stderr_file)

            local compile_rc = execMerged(ens_exe,
                {"build", job.source, "--output", exe_file}, compile_log)
            local compile_log_text = io.readfile(compile_log) or ""

            if expected_error ~= nil then
                local why = {}
                if os.isfile(exe_file) then
                    table.insert(why, "compile succeeded but @expect-error was set")
                end
                if not compile_log_text:find(expected_error, 1, true) then
                    table.insert(why, string.format("error %q not found in stderr", expected_error))
                end
                if #why == 0 then
                    return {name = name, ok = true}
                end
                local short = table.concat(why, "; ")
                return {name = name, ok = false, short = short,
                    full = string.format("%s: %s\n%s", name, short,
                        compile_log_text:gsub("[\r\n]+$", ""))}
            end

            if not os.isfile(exe_file) then
                return {name = name, ok = false, short = "compile failed",
                    full = string.format("%s: compile failed (exit %s)\n%s",
                        name, tostring(compile_rc),
                        compile_log_text:gsub("[\r\n]+$", ""))}
            end

            -- a controlled environment for the getenv-based ffi fixture: a dedicated variable set
            -- to a known value, and its unset counterpart cleared, so the fixture never depends on
            -- ambient variables such as PATH.
            local fixture_env = os.getenvs()
            fixture_env.ENS_TEST_FROMCSTRING_PRESENT = "hermetic"
            fixture_env.ENS_TEST_FROMCSTRING_ABSENT = nil
            local run_rc = execSplit(exe_file, {}, stdout_file, stderr_file, {envs = fixture_env})
            local why = compareRun(run_rc, captured(stdout_file), captured(stderr_file))

            if #why == 0 then
                return {name = name, ok = true}
            end
            local short = table.concat(why, "; ")
            return {name = name, ok = false, short = short,
                full = string.format("%s: %s", name, short)}
        end

        -- worker count: ENS_TEST_JOBS override, else one per cpu, capped at the test count.
        local njob = tonumber(os.getenv("ENS_TEST_JOBS"))
        if not njob then
            local cpuinfo = os.cpuinfo()
            njob = (type(cpuinfo) == "table" and cpuinfo.ncpu) or 8
        end
        njob = math.max(1, math.min(njob, #jobs))

        print(string.format("Running %d tests with %d parallel jobs...", #jobs, njob))

        local results = {}
        runjobs("run-tests", function (index)
            local r = run_one(jobs[index])
            results[index] = r
            if r.ok then
                print(string.format("\27[32mPASS\27[0m %s%s", r.name,
                    r.note and (" (" .. r.note .. ")") or ""))
            else
                print(string.format("\27[31mFAIL\27[0m %s - %s", r.name, r.short))
            end
        end, {total = #jobs, comax = njob})

        -- tally in job order for a stable summary regardless of completion order.
        local total = #jobs
        local passed = 0
        local failures = {}
        for i = 1, total do
            local r = results[i]
            if r and r.ok then
                passed = passed + 1
            else
                table.insert(failures, (r and r.full) or
                    string.format("%s: no result", jobs[i].name))
            end
        end

        print(string.format("\n%d/%d tests passed", passed, total))
        if passed < total then
            for _, f in ipairs(failures) do print("  - " .. f) end
            os.raise("%d test failure(s)", total - passed)
        end
    end)
