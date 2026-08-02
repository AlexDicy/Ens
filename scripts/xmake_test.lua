-- compile each tests/*.ens with `ens` and verify the exit code, the standard output and the
-- standard error against the directives in the test source header:
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
-- use @expect-error instead to assert the compiler reports a specific diagnostic. These accumulate:
-- every one of them names a substring the compiler's output must contain, so a file whose problems
-- are reported together says so by listing them all.
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
        local build_dir = path.join(os.projectdir(), "build", plat, arch, mode)
        local exe_suffix = is_host("windows") and ".exe" or ""
        -- what `ens` names the artifacts it is not told the name of: it follows the target's object
        -- format, and every assertion about a default-named file has to as well.
        local obj_suffix = is_host("windows") and ".obj" or ".o"
        -- where `ens` puts a workspace member's program: with its objects under the build root,
        -- where it cannot land on the member folder it was built from. The triple in that path is
        -- this machine's own, so it is read back rather than named here.
        local function member_artifact(workspace, stem)
            local found = os.files(path.join(workspace, ".ens", "*", "O*",
                stem .. exe_suffix))
            return found[1] or path.join(workspace, ".ens", "<target>", "O2", stem .. exe_suffix)
        end
        -- the linker bridge every Ens program links through. It is a separate target from the
        -- compiler, so a fresh clone needs it built before anything can reach an executable.
        local lld_library = path.join(build_dir, is_host("windows") and "ens-lld.dll"
            or (is_host("macosx") and "libens-lld.dylib" or "libens-lld.so"))
        if not os.isfile(lld_library) then
            print("Building the ens-lld linker bridge...")
            os.exec("xmake build ens-lld")
        end
        if not os.isfile(lld_library) then
            os.raise("Could not locate the linker bridge at " .. lld_library)
        end

        -- the seed: the committed `ens` for this host. Its one job is to build the compiler out of
        -- this tree, and nothing else in the suite runs it. Nothing it produces is ever compared
        -- against anything either, so a seed left behind by the tree shows up as a failure to build
        -- rather than as drift in what the tests measure.
        local seed_platforms = {windows = "windows", linux = "linux", macosx = "macos"}
        local seed_architectures = {x64 = "x64", x86_64 = "x64", arm64 = "arm64"}
        local seed_host = (seed_platforms[plat] or plat) .. "-" .. (seed_architectures[arch] or arch)
        local seed_dir = path.join(os.projectdir(), "build", "seed")
        local seed_exe = path.join(seed_dir, "ens" .. exe_suffix)

        -- the compiler under test: `ens` as this tree defines it, and what every job that compiles
        -- Ens drives. It is built below, before the jobs start, because they run in parallel and
        -- every one of them needs it.
        local host_dir = path.join(os.projectdir(), "build", "host")
        local host_exe = path.join(host_dir, "ens" .. exe_suffix)

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

        -- the standard library's own unit tests: the rules its modules answer with rather than the
        -- effects the tests/ fixtures drive against a real operating system. The environment merge
        -- is checked for every platform here, since it is handed the platform's name.
        if want("std_library") then
            table.insert(jobs, {
                name = "std_library",
                source = path.join(os.projectdir(), "libs", "std"),
                ens_test_args = {},
            })
        end

        -- the language server is the one target nothing else here compiles, so it can stop building
        -- without a single check noticing.
        if want("ens_lsp") then
            table.insert(jobs, {
                name = "ens_lsp",
                lsp_build = true,
            })
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

        -- the self-hosted semantic layer's tests.
        if want("selfhost_sema") then
            table.insert(jobs, {
                name = "selfhost_sema",
                source = path.join(os.projectdir(), "selfhost", "sema"),
                ens_test_args = {},
            })
        end

        -- the host library's tests: the environment snapshot a run reads once, the folders Ens keeps
        -- for a user, and the scratch folder a run builds in and then removes.
        if want("selfhost_host") then
            table.insert(jobs, {
                name = "selfhost_host",
                source = path.join(os.projectdir(), "selfhost", "host"),
                ens_test_args = {},
            })
        end

        -- the command-line library's tests: every rule of the grammar a command line is read
        -- with, every usage problem it reports, and the generated help pinned by goldens.
        if want("selfhost_cli") then
            table.insert(jobs, {
                name = "selfhost_cli",
                source = path.join(os.projectdir(), "selfhost", "cli"),
                ens_test_args = {},
            })
        end

        -- the packaging library's tests: sha256 against the digests NIST publishes, the content
        -- store's layout and publishing, ens.lock round-tripping byte for byte, and the byte-exact
        -- edits an override makes to ens.overrides.
        if want("selfhost_packages") then
            table.insert(jobs, {
                name = "selfhost_packages",
                source = path.join(os.projectdir(), "selfhost", "packages"),
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

        -- the semantic gate runs the self-hosted sema pipeline over every
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

        -- the code-generation gate: build the spike (an Ens program that emits an
        -- object file through the ens.llvm binding) and the harness, then drive the spike from
        -- emission through linking and execution and enforce the skip-list anti-rot rule.
        --
        -- It runs once per shipped code-generation configuration. '-O0' is not a debug mode nobody
        -- ships: it is what '-O0' compiles every program with, so it is gated exactly as hard as
        -- the default is, with the same empty skip list.
        if want("codegencheck") then
            table.insert(jobs, {
                name = "codegencheck",
                codegencheck = true,
                optimization = "2",
            })
        end

        if want("codegencheck_unoptimized") then
            table.insert(jobs, {
                name = "codegencheck_unoptimized",
                codegencheck = true,
                optimization = "0",
            })
        end

        -- the bootstrap fixpoint: the self-hosted driver compiles itself twice with identical
        -- objects, pushing the whole selfhost corpus through the self-hosted pipeline both
        -- times.
        if want("bootstrap") then
            table.insert(jobs, {
                name = "bootstrap",
                bootstrap = true,
            })
        end

        -- the ens.llvm binding's own unit tests: build small LLVM modules through the binding and
        -- assert the LLVM verifier accepts them. They link the native LLVM-C library, so the job
        -- points the linker and loader at the local LLVM package the same way codegencheck does.
        if want("selfhost_llvm") then
            table.insert(jobs, {
                name = "selfhost_llvm",
                source = path.join(os.projectdir(), "selfhost", "llvm"),
                ens_test_args = {},
            })
        end

        -- the self-hosted code generator's unit tests: EIR construction, golden dumps, the
        -- structural verifier, and lowering over analyzed programs. The package depends on the
        -- native ens.llvm binding, so the job carries the same LLVM plumbing.
        if want("selfhost_codegen") then
            table.insert(jobs, {
                name = "selfhost_codegen",
                source = path.join(os.projectdir(), "selfhost", "codegen"),
                ens_test_args = {},
            })
        end

        -- the linking library's unit tests: the link line of each flavor pinned argument by
        -- argument, the flavor a triple picks, and the paths a failing link reports through. It
        -- binds the ens-lld native library, so the job carries the same native plumbing.
        if want("selfhost_link") then
            table.insert(jobs, {
                name = "selfhost_link",
                source = path.join(os.projectdir(), "selfhost", "link"),
                ens_test_args = {},
            })
        end

        -- the build library's unit tests: target resolution, the member order a workspace's graph
        -- implies, the version its members must agree on, the source scan, native library mapping,
        -- stdlib discovery, and the artifact naming of each target. It pulls in ens.codegen and
        -- with it the native ens.llvm binding, so the job carries the same LLVM plumbing.
        if want("selfhost_build") then
            table.insert(jobs, {
                name = "selfhost_build",
                source = path.join(os.projectdir(), "selfhost", "build"),
                ens_test_args = {},
            })
        end

        -- the self-hosted driver's unit tests: the command surface it declares and the invocation
        -- a command line settles into. The package pulls in ens.codegen and with it the native
        -- ens.llvm binding, so the job carries the same LLVM plumbing.
        if want("selfhost_driver") then
            table.insert(jobs, {
                name = "selfhost_driver",
                source = path.join(os.projectdir(), "selfhost", "driver"),
                ens_test_args = {},
            })
        end

        -- the Ens-written command's own behavior: its help, the argv it refuses, each optimization
        -- level, a library against an application, the order a workspace's members build in, and a
        -- workspace whose members disagree on the Ens version.
        if want("cli_build") then
            table.insert(jobs, {
                name = "cli_build",
                cli_build = true,
            })
        end

        -- the two commands that build something only in order to run it: the arguments `run` hands
        -- a program, the code it propagates, the folder neither of them leaves behind, and the
        -- rules `test` applies to what it discovers.
        if want("cli_runtest") then
            table.insert(jobs, {
                name = "cli_runtest",
                cli_runtest = true,
            })
        end

        -- the override subcommands of the Ens-written command: the byte-exact edits each form makes
        -- to ens.overrides, the folder it refuses, what 'list' says about a target that works and one
        -- that does not, and a build resolving through the redirection.
        if want("cli_overriding") then
            table.insert(jobs, {
                name = "cli_overriding",
                cli_overriding = true,
            })
        end

        -- git-sourced dependencies through the Ens-written command: tag resolution in each of its
        -- spellings, fetching into the cache, ens.lock written and kept current, a build that asks
        -- the network nothing, '--offline' and '--locked', the highest version required across
        -- transitive requirements, and every rejection - a moved tag, submodules, a span of majors,
        -- two repositories for one package, a tag that declares something else.
        if want("cli_dependencies") then
            table.insert(jobs, {
                name = "cli_dependencies",
                cli_dependencies = true,
            })
        end

        -- the prebuilt native libraries a manifest binds instead of naming a library: downloaded
        -- once over a file:// URL, checked against its digest, reused from the cache under
        -- '--offline', refused when the digest does not match, and recorded in ens.lock.
        if want("cli_prebuilt") then
            table.insert(jobs, {
                name = "cli_prebuilt",
                cli_prebuilt = true,
            })
        end

        -- the version multiplexer, driven end to end against a second toolchain installed in a
        -- scratch folder: the hop, the command line it forwards, the exit code it propagates, both
        -- ways of suppressing it, and the guard that stops a delegate delegating again.
        if want("cli_toolchain") then
            table.insert(jobs, {
                name = "cli_toolchain",
                cli_toolchain = true,
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

        -- find the native libraries an Ens build links against - the local LLVM the compiler was
        -- built against, and the ens-lld linker bridge from this build - and build the environment a
        -- process linking or loading them needs: on Windows lld-link reads LIB (and finds nothing
        -- else once LIB is set, so the MSVC and SDK folders must be added too) and the loader
        -- searches PATH; on Linux/macOS the ens linker turns LIBRARY_PATH into -L and -rpath, and
        -- the loader honors LD_LIBRARY_PATH / DYLD_FALLBACK_LIBRARY_PATH. A machine without the
        -- LLVM library cannot build the compiler either, so a clear hard failure is the right
        -- outcome.
        -- Returns env, the shared libraries to place beside an executable, nil, or nil, nil, message.
        local function llvmEnvironment()
            local on_windows = is_host("windows")
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
                return nil, nil, string.format("could not find %s under %s; install the LLVM "
                    .. "package the compiler builds against", looked, packages)
            end
            local env = os.getenvs()
            if on_windows then
                local msvc = toolchain.load("msvc", {plat = plat, arch = arch})
                local msvc_lib = ""
                if msvc then
                    local envs = msvc:runenvs()
                    if envs and envs.LIB then msvc_lib = envs.LIB end
                end
                env.LIB = msvc_lib .. ";" .. llvm_lib .. ";" .. build_dir
                env.PATH = llvm_bin .. ";" .. build_dir .. ";" .. (env.PATH or "")
                return env, {path.join(llvm_bin, "LLVM-C.dll"), lld_library}, nil
            end
            local function prepend(current, dir)
                if current and #current > 0 then return dir .. ":" .. current end
                return dir
            end
            env.LIBRARY_PATH = prepend(prepend(env.LIBRARY_PATH, llvm_lib), build_dir)
            if is_host("macosx") then
                -- the fallback list rather than DYLD_LIBRARY_PATH, which is consulted first and
                -- by file name alone: the LLVM package ships its own libc++, and shadowing the
                -- system one leaves every process started here without the C++ runtime this
                -- macOS builds against.
                env.DYLD_FALLBACK_LIBRARY_PATH = prepend(prepend(
                    env.DYLD_FALLBACK_LIBRARY_PATH or "/usr/local/lib:/usr/lib", llvm_lib),
                    build_dir)
            else
                env.LD_LIBRARY_PATH = prepend(prepend(env.LD_LIBRARY_PATH, llvm_lib), build_dir)
            end
            return env, {}, nil
        end

        -- the Windows loader searches an executable's own folder first, so a shared library an Ens
        -- program links has to sit beside it; on Linux/macOS the rpath the linker wrote and the
        -- library-path environment cover it and the list is empty.
        local function placeNativeLibraries(libraries, folder)
            for _, library in ipairs(libraries) do
                os.cp(library, path.join(folder, path.filename(library)))
            end
        end

        -- place the seed: the `ens` committed for this host, put where the build below looks for it.
        -- A host with no committed seed cannot run the suite, and saying so is the right outcome:
        -- falling back to another compiler would leave the seed untested.
        local function locateSeed()
            local committed = path.join(os.projectdir(), "seed", seed_host, "ens" .. exe_suffix)
            if not os.isfile(committed) then
                os.raise("no bootstrap seed for %s: %s does not exist. Build one on that platform "
                    .. "with an `ens` that already runs there (`ens build selfhost/driver "
                    .. "--output <path>`) and commit it there.", seed_host, committed)
            end
            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                os.raise("could not place the seed compiler: %s", env_error)
            end
            os.tryrm(seed_dir)
            os.mkdir(seed_dir)
            os.cp(committed, seed_exe)
            if not is_host("windows") then
                -- a copy is not guaranteed to keep the executable bit the checkout recorded.
                os.runv("chmod", {"+x", seed_exe})
            end
            -- the seed loads LLVM and the linker bridge at run time, and on Windows the loader
            -- looks beside the executable first.
            placeNativeLibraries(native_libraries, seed_dir)
        end

        -- build the compiler every job then drives: the seed compiles `selfhost/driver` out of this
        -- tree, so what the suite measures is the sources rather than a binary in the repository.
        -- This is also the whole of what the committed seed has to do, and it is checked here by
        -- construction: a seed that cannot build the tree stops the run before any job starts.
        local function buildHostCompiler()
            local log = path.join(out_dir, "host.log")
            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                os.raise("could not build the compiler: %s", env_error)
            end
            os.tryrm(host_dir)
            os.mkdir(host_dir)
            local rc = execMerged(seed_exe, {"build",
                path.join(os.projectdir(), "selfhost", "driver"), "--output", host_exe,
                "--stdlib", path.join(os.projectdir(), "libs"), "--objects", host_dir}, log,
                {envs = env})
            if rc ~= 0 or not os.isfile(host_exe) then
                os.raise("the %s seed could not build the compiler from this tree (exit %s). A seed "
                    .. "older than the sources has to be replaced:\n%s", seed_host, tostring(rc),
                    captured(log))
            end
            placeNativeLibraries(native_libraries, host_dir)
        end

        -- the corpus round-trip harness: build the harness exe fresh from its own workspace (it
        -- imports the front end as the @ens.frontend package), enumerate every .ens file and every
        -- ens.package/ens.overrides manifest in the real source trees, and run the harness over
        -- the list.
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
            local env = llvmEnvironment()
            local compile_rc = execMerged(host_exe, {"build", corpus_src, "--output", exe_file},
                log, {envs = env})
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

        -- the semantic gate: build the harness exe fresh from its own workspace (it
        -- imports the front end and the sema layer as packages), enumerate every program unit into
        -- a manifest, and run the harness over it. Units are the single-file tests, the folder
        -- tests, and the selfhost library packages; libs/std is covered transitively by every unit
        -- that imports @std.
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
            local env = llvmEnvironment()
            local compile_rc = execMerged(host_exe, {"build", check_src, "--output", exe_file}, log,
                {envs = env})
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
                              "llvm", "codegen", "codegencheck", "host", "cli", "packages",
                              "link", "build", "driver"}) do
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

        -- the code-generation gate: build the harness and the spike, drive the
        -- spike from object emission through linking and execution, and enforce the skip-list
        -- anti-rot rule over the runnable fixtures. The spike links the ens.llvm native binding,
        -- so the build has to point the linker at the local LLVM library and the run has to make
        -- the shared library loadable.
        local function run_codegencheck(job)
            local name = job.name
            local check_dir = path.join(os.projectdir(), "build",
                "codegencheck-O" .. job.optimization)
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

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end

            -- the harness itself links the native LLVM binding through ens.codegen, so its
            -- build needs the same linker environment as the spike.
            --
            -- Both arms build these same two packages, at the same time and in the same
            -- configuration, so both are told where to put their object files: left to itself a
            -- build keeps them under the build root, which the two arms would then share and write
            -- over each other in. Only a harness runs the same build twice at once like this, and
            -- '--objects' is what says otherwise.
            local harness_objects = path.join(check_dir, "objects-harness")
            local spike_objects = path.join(check_dir, "objects-spike")
            local harness_rc = execMerged(host_exe,
                {"build", check_src, "--output", harness_exe, "--objects", harness_objects},
                log, {envs = env})
            if not os.isfile(harness_exe) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s", name,
                        tostring(harness_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end
            local spike_rc = execMerged(host_exe, {"build", spike_src, "--output", spike_exe,
                "--objects", spike_objects}, log, {envs = env})
            if not os.isfile(spike_exe) then
                return {name = name, ok = false, short = "spike build failed",
                    full = string.format("%s: spike build failed (exit %s)\n%s", name,
                        tostring(spike_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end
            placeNativeLibraries(native_libraries, check_dir)

            -- list every candidate fixture, naming the file whose header carries the directives;
            -- the harness reads each one and works out for itself what the fixture asks for.
            local lines = {
                "spike " .. spike_exe,
                "stdlib " .. (path.join(os.projectdir(), "libs"):gsub("\\", "/")),
                "scratch " .. scratch,
                "skiplist " .. skiplist,
                "optimization " .. job.optimization,
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
                    table.insert(lines, "fixture tests/" .. path.basename(sub) .. " " .. directive)
                end
            end
            io.writefile(manifest, table.concat(lines, "\n") .. "\n")

            -- the harness runs the spike as a child, so it needs the same library-path
            -- environment for the shared library to load. The fixtures it runs inherit the
            -- same controlled variables the fixture runner sets, so the getenv-based ffi
            -- fixture stays hermetic here too.
            env.ENS_TEST_FROMCSTRING_PRESENT = "hermetic"
            env.ENS_TEST_FROMCSTRING_ABSENT = nil
            local started = os.mclock()
            local run_rc = execMerged(harness_exe, {manifest}, log, {envs = env})
            local seconds = (os.mclock() - started) / 1000.0
            local out = captured(log)
            if run_rc == 0 then
                return {name = name, ok = true,
                    note = string.format("-O%s, %.0fs", job.optimization, seconds)}
            end
            return {name = name, ok = false,
                short = string.format("harness exit %s", tostring(run_rc)),
                full = string.format("%s:\n%s", name, out)}
        end

        -- the bootstrap fixpoint: the Ens-written compiler compiles itself, twice, with identical
        -- output. Stage 2 has the compiler under test compile its own sources (stage2/ens plus one
        -- object per module), stage 3 has stage2's ens compile the same sources again. The gate
        -- holds when the two stages' objects hold the same modules with identical bytes; the
        -- executables are compared as a note only, because linker output may carry timestamps.
        --
        -- Neither side of that comparison was produced by the seed, so what the seed generates
        -- cannot decide it: the gate answers for this tree's code generation and nothing else.
        local function run_bootstrap(job)
            local name = job.name
            local boot_dir = path.join(os.projectdir(), "build", "bootstrap")
            local stage2_dir = path.join(boot_dir, "stage2")
            local stage3_dir = path.join(boot_dir, "stage3")
            local boot2 = path.join(stage2_dir, "ens" .. exe_suffix)
            local boot3 = path.join(stage3_dir, "ens" .. exe_suffix)
            local log = path.join(out_dir, name .. ".log")
            local driver_src = path.join(os.projectdir(), "selfhost", "driver")
            local libs = path.join(os.projectdir(), "libs")

            os.tryrm(boot_dir)
            os.mkdir(boot_dir)
            os.mkdir(stage2_dir)
            os.mkdir(stage3_dir)

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end
            -- stage2's ens loads LLVM and the linker bridge at run time.
            placeNativeLibraries(native_libraries, stage2_dir)

            local seconds = {}
            local function staged(stage, program, argv, produced)
                local started = os.mclock()
                local rc = execMerged(program, argv, log, {envs = env})
                table.insert(seconds, (os.mclock() - started) / 1000.0)
                if rc ~= 0 or not os.isfile(produced) then
                    return string.format("%s failed (exit %s)\n%s", stage, tostring(rc),
                        (io.readfile(log) or ""):gsub("[\r\n]+$", ""))
                end
                return nil
            end

            local failed = staged("stage 2 (the compiler compiles itself)", host_exe,
                    {"build", driver_src, "--output", boot2, "--stdlib", libs,
                     "--objects", stage2_dir}, boot2)
                or staged("stage 3 (stage 2 compiles the same sources)", boot2,
                    {"build", driver_src, "--output", boot3, "--stdlib", libs,
                     "--objects", stage3_dir}, boot3)
            if failed then
                return {name = name, ok = false, short = failed:match("^[^\n]+"),
                    full = string.format("%s: %s", name, failed)}
            end

            -- the fixpoint: the two stages' objects, module by module in name order.
            local function objectNames(folder)
                local names = {}
                for _, f in ipairs(os.files(path.join(folder, "*" .. obj_suffix))) do
                    table.insert(names, path.filename(f))
                end
                table.sort(names)
                return names
            end
            local function readBytes(file)
                local handle = io.open(file, "rb")
                if not handle then return "" end
                local bytes = handle:read("*a")
                handle:close()
                return bytes or ""
            end
            local names2 = objectNames(stage2_dir)
            local names3 = objectNames(stage3_dir)
            local present = {}
            for _, n in ipairs(names2) do present[n] = true end
            for _, n in ipairs(names3) do
                if not present[n] then
                    return {name = name, ok = false,
                        short = string.format("stage 3 emitted %s, stage 2 did not", n),
                        full = string.format("%s: stage 3 emitted %s, stage 2 did not", name, n)}
                end
                present[n] = nil
            end
            for _, n in ipairs(names2) do
                if present[n] then
                    return {name = name, ok = false,
                        short = string.format("stage 2 emitted %s, stage 3 did not", n),
                        full = string.format("%s: stage 2 emitted %s, stage 3 did not", name, n)}
                end
            end
            local total_bytes = 0
            for _, n in ipairs(names2) do
                local bytes2 = readBytes(path.join(stage2_dir, n))
                local bytes3 = readBytes(path.join(stage3_dir, n))
                total_bytes = total_bytes + #bytes2
                if bytes2 ~= bytes3 then
                    return {name = name, ok = false,
                        short = string.format("fixpoint failed at %s", n),
                        full = string.format("%s: %s differs between stage 2 (%d bytes) and "
                            .. "stage 3 (%d bytes); the pipeline is not deterministic or the "
                            .. "two compilers disagree", name, n, #bytes2, #bytes3)}
                end
            end
            local executables = readBytes(boot2) == readBytes(boot3) and "identical"
                or "differ (not gating: linker output)"
            local note = string.format("stages %.0fs/%.0fs; fixpoint over %d modules, "
                .. "%.1f MB; executables %s", seconds[1] or 0, seconds[2] or 0,
                #names2, total_bytes / (1024 * 1024), executables)
            return {name = name, ok = true, note = note}
        end

        -- the command's own behavior, driven end to end against scratch fixtures: every assertion
        -- here is written to this command's own contract.
        local function run_cli_build(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "ensbuild")
            os.tryrm(root)
            os.mkdir(root)
            local work = path.join(root, "work")
            os.mkdir(work)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env = llvmEnvironment()

            -- run the command and assert its exit code and the fragments its output must carry; a
            -- fragment prefixed '!' must not appear.
            local function run(argv, opt, expected_rc, ...)
                local options = {envs = env}
                for key, value in pairs(opt or {}) do options[key] = value end
                local rc = execMerged(host_exe, argv, log, options)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("ens %s: output must not carry "
                                .. "%q\n%s", label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
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
                local out = captured(log)
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            local hello = path.join(tests_dir, "hello.ens")
            local in_work = {curdir = work}
            io.writefile(path.join(root, "notes.txt"), "not Ens source\n")
            io.writefile(path.join(root, "broken.ens"), "main() -> int {\n    return 0;\n")

            -- help and version
            run({"--help"}, nil, 0, "Usage: ens <command>", "build", "check", "version",
                "!cst-dump")
            run({"-h"}, nil, 0, "Usage: ens <command>")
            run({"help"}, nil, 0, "Usage: ens <command>", "build", "check", "version",
                "!cst-dump")
            run({"help", "build"}, nil, 0, "Usage: ens build", "-o, --output <file>",
                "-O, --optimization-level <level>", "!--objects")
            run({"version"}, nil, 0, "ens 0.1")
            run({"--version"}, nil, 0, "ens 0.1")
            run({}, nil, 2, "no command given")

            -- argv the command refuses, each one naming what to write instead
            run({"frobnicate"}, nil, 2, "unknown command 'frobnicate'")
            run({"biuld", hello}, nil, 2, "did you mean 'build'?")
            run({"build", "--outpu", hello}, nil, 2, "did you mean '--output'?")
            run({"build", "--output"}, nil, 2, "needs a value", "-o<file>")
            run({"build", "-O"}, nil, 2, "-O<level>", "--optimization-level <level>")
            run({"build", "--optimization-level=9"}, nil, 2, "takes a level from 0 to 3")
            run({"build", "-q", "-v"}, nil, 2, "opposite things")
            run({"build", "--target", "pdp11-dec-unix"}, nil, 2, "pdp11-dec-unix")
            run({"build", hello, "extra"}, nil, 2, "unexpected argument 'extra'")
            run({"build", path.join(tests_dir, "no_such_place")}, nil, 2, "does not exist")
            run({"build", path.join(root, "notes.txt")}, nil, 2, "not an Ens source file")
            run({"build"}, in_work, 2, "no ens.package manifest was found")

            -- a single file: named output, then the default name in the folder the command ran in
            local hello_exe = path.join(root, "hello_out.exe")
            run({"build", hello, "--output", hello_exe}, nil, 0, "built")
            run_program(hello_exe, 0, "Hello, world!")
            run({"build", hello}, in_work, 0, "built")
            run_program(path.join(work, "hello" .. exe_suffix), 0, "Hello, world!")

            -- every optimization level is a shipped configuration, so every one is run
            for _, level in ipairs({"-O0", "-O1", "-O2", "-O3"}) do
                local leveled = path.join(root, "hello" .. level .. ".exe")
                run({"build", hello, level, "--output", leveled}, nil, 0, "built")
                run_program(leveled, 0, "Hello, world!")
            end

            -- the object files a build emits are kept under the build root, in a folder of their
            -- own per target and per optimization level. The triple is this machine's own, so the
            -- folders are read back rather than named here.
            local function objectFolders(base)
                local found = {}
                for _, triple in ipairs(os.dirs(path.join(base, ".ens", "*"))) do
                    for _, level in ipairs(os.dirs(path.join(triple, "*"))) do
                        table.insert(found, path.filename(triple) .. "/" .. path.filename(level))
                    end
                end
                table.sort(found)
                return found
            end
            local function expectFolders(base, expected, after)
                local found = table.concat(objectFolders(base), " ")
                if found ~= expected then
                    table.insert(failures, string.format("%s: objects are under %q, expected %q",
                        after, found, expected))
                end
            end

            local objects = path.join(root, "objects")
            os.mkdir(path.join(objects, "src"))
            io.writefile(path.join(objects, "ens.package"),
                'package demo.objects {\n    ens "0.1";\n}\n')
            io.writefile(path.join(objects, "src", "main.ens"),
                'main() -> int {\n    print("objects");\n    return 0;\n}\n')

            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, "built")
            local triple = path.filename(os.dirs(path.join(objects, ".ens", "*"))[1] or "none")
            expectFolders(objects, triple .. "/O2", "a default build")
            if #os.files(path.join(objects, ".ens", "*", "O2", "*" .. obj_suffix)) == 0 then
                table.insert(failures, "a build left no object files under the build root")
            end

            -- each object is assembled in a folder of the build's own and moved into place once it
            -- is whole, so nothing can read one half-written. That the move is indivisible cannot be
            -- asserted without racing two builds; what is asserted is that the staging folder is
            -- always taken away, and that a build whose objects are already there replaces them.
            local function expectNoStaging(base, after)
                local left = os.dirs(path.join(base, "staging-*"))
                if #left > 0 then
                    table.insert(failures, string.format("%s left %s behind", after,
                        path.filename(left[1])))
                end
            end
            expectNoStaging(path.join(objects, ".ens", triple, "O2"), "a build")
            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, "built")
            expectNoStaging(path.join(objects, ".ens", triple, "O2"), "a build over its own objects")
            run_program(path.join(root, "objects.exe"), 0, "objects")

            -- an edited source reaches the object at its path rather than the previous one staying
            io.writefile(path.join(objects, "src", "main.ens"),
                'main() -> int {\n    print("objects again");\n    return 0;\n}\n')
            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, "built")
            run_program(path.join(root, "objects.exe"), 0, "objects again")

            -- and nowhere else: not beside the executable, and not in the folder it was run from
            for _, elsewhere in ipairs({root, work}) do
                if #os.files(path.join(elsewhere, "*" .. obj_suffix)) > 0 then
                    table.insert(failures, string.format("object files were left in %s", elsewhere))
                end
            end

            -- a second level does not share the first one's folder, so an object built at one level
            -- can never be picked up by a build at another
            run({"build", objects, "-O0", "--output", path.join(root, "objects0.exe")}, nil, 0,
                "built")
            expectFolders(objects, triple .. "/O0 " .. triple .. "/O2", "a build at another level")

            -- the folder says to ignore everything under it, so nothing has to be added to a
            -- .gitignore anywhere and nobody has to remember it
            local ignored = io.readfile(path.join(objects, ".ens", ".gitignore")) or ""
            if ignored:gsub("%s+", "") ~= "*" then
                table.insert(failures, string.format("the artifacts folder ignores %q", ignored))
            end

            -- '--objects' still decides where they go, which is what the bootstrap gate relies on
            local named_objects = path.join(root, "named-objects")
            os.mkdir(named_objects)
            run({"build", objects, "--objects", named_objects, "--output",
                path.join(root, "objects2.exe")}, nil, 0, "built")
            if #os.files(path.join(named_objects, "*" .. obj_suffix)) == 0 then
                table.insert(failures, "'--objects' did not decide where the objects went")
            end
            expectNoStaging(named_objects, "a build into a named objects folder")

            -- quiet says nothing on success, verbose says what it did, and --explain-arc accounts.
            -- The quiet run links with only this build's own folder on the library path: the LLVM
            -- package's folder holds a libunwind newer than the deployment target, which the linker
            -- rightly remarks on and which is not what is being asserted here.
            local quiet_env = {}
            for key, value in pairs(env) do quiet_env[key] = value end
            quiet_env.LIBRARY_PATH = build_dir
            local _, quiet_out = run({"build", hello, "--output",
                path.join(root, "quiet.exe"), "-q"}, {envs = quiet_env}, 0)
            if quiet_out:gsub("%s+", "") ~= "" then
                table.insert(failures, string.format("ens build -q said %q", quiet_out))
            end
            run({"build", hello, "--output", path.join(root, "loud.exe"), "-v"}, nil, 0,
                "emitted", "object file(s)")
            run({"build", hello, "--output", path.join(root, "arc.exe"), "--explain-arc"}, nil, 0,
                "elided across the program")

            -- an application package is named after its package; a library keeps no artifact and
            -- refuses an output, while keeping its object files the way a program keeps its own.
            -- The library's build root is in the fixture tree, so what an earlier run left there
            -- goes first and the assertion is about this run.
            run({"build", path.join(tests_dir, "pkg_import_main")}, in_work, 0, "built")
            run_program(path.join(work, "main" .. exe_suffix), 0, "Hello, Ada! [acme.tools]")
            local library_root = path.join(tests_dir, "pkg_import_dep")
            os.tryrm(path.join(library_root, ".ens"))
            run({"build", library_root}, in_work, 0, "as a library")
            if os.isfile(path.join(work, "tools" .. exe_suffix)) then
                table.insert(failures, "building a library left an executable behind")
            end
            if #os.files(path.join(library_root, ".ens", "*", "O2", "*" .. obj_suffix)) == 0 then
                table.insert(failures, "a library build left no object files under the build root")
            end
            if #os.dirs(path.join(work, ".ens-library-*")) > 0 then
                table.insert(failures, "a library build left a folder of its own beside the command")
            end
            run({"build", path.join(tests_dir, "pkg_import_dep"), "--output",
                path.join(root, "tools.exe")}, nil, 2, "builds as a library")

            -- check writes nothing and reports what it found
            run({"check", hello}, nil, 0, "nothing to report")
            run({"check", path.join(tests_dir, "pkg_import_dep")}, nil, 0, "nothing to report")
            run({"check", path.join(tests_dir, "undefined_function.ens")}, nil, 1,
                "Undefined function")

            -- a workspace root builds every member, the library before the application that
            -- depends on it even though the manifest lists the application first
            local workspace = path.join(tests_dir, "cli_workspace")
            run({"build", workspace}, in_work, 0, "[1/2] demo.lib: compiled",
                "[2/2] demo.app: built")
            run({"build", workspace, "--output", path.join(root, "x.exe")}, nil, 2, "--output")
            run({"check", workspace}, nil, 0, "[1/2] demo.lib", "[2/2] demo.app")

            -- a workspace whose members disagree on the Ens version is refused, naming both
            local split = path.join(root, "split")
            os.mkdir(path.join(split, "app", "src"))
            os.mkdir(path.join(split, "lib", "src"))
            io.writefile(path.join(split, "ens.package"),
                'workspace {\n    member "app";\n    member "lib";\n}\n')
            io.writefile(path.join(split, "app", "ens.package"),
                'package split.app {\n    ens "0.1";\n}\n')
            io.writefile(path.join(split, "app", "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            io.writefile(path.join(split, "lib", "ens.package"),
                'package split.lib {\n    ens "0.2";\n}\n')
            io.writefile(path.join(split, "lib", "src", "greet.ens"),
                'export greet() -> string {\n    return "hi";\n}\n')
            run({"build", split}, nil, 2, "disagree on the Ens version", "split.app", "split.lib",
                '"0.1"', '"0.2"')

            -- the hidden syntax tools
            run({"cst-dump", hello}, nil, 0, "SourceFile")
            run({"cst-analyze", hello}, nil, 0)
            run({"cst-analyze", path.join(root, "broken.ens")}, nil, 1, "broken.ens:")
            run({"cst-dump", path.join(root, "gone.ens")}, nil, 2, "could not read")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- `ens run` and `ens test`: the two commands that build something only in order to run it.
        -- Both work in a folder under the system's temp directory and remove it afterwards, which
        -- this job points at a scratch folder of its own so it can assert nothing is left there.
        local function run_cli_runtest(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "ensruntest")
            os.tryrm(root)
            os.mkdir(root)
            local temp = path.join(root, "temp")
            os.mkdir(temp)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local base = llvmEnvironment()
            base.TMP = temp
            base.TEMP = temp
            base.TMPDIR = temp

            -- run the command and assert its exit code and the fragments its output must carry; a
            -- fragment prefixed '!' must not appear.
            local function run(argv, opt, expected_rc, ...)
                local options = {envs = base}
                for key, value in pairs(opt or {}) do options[key] = value end
                local rc = execMerged(host_exe, argv, log, options)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("ens %s: output must not carry "
                                .. "%q\n%s", label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            local function assertEmptyFolder(folder, label, after)
                local left = {}
                for _, entry in ipairs(os.dirs(path.join(folder, "*"))) do
                    table.insert(left, path.filename(entry))
                end
                for _, entry in ipairs(os.files(path.join(folder, "*"))) do
                    table.insert(left, path.filename(entry))
                end
                if #left > 0 then
                    table.insert(failures, string.format("%s left %s under the %s",
                        after, table.concat(left, ", "), label))
                    os.tryrm(path.join(folder, "*"))
                end
            end

            -- nothing may be left under the temp directory once a command has finished.
            local function assertTempEmpty(after)
                assertEmptyFolder(temp, "temp folder", after)
            end

            local function writePackage(folder, manifest, files)
                os.mkdir(path.join(folder, "src"))
                io.writefile(path.join(folder, "ens.package"), manifest)
                for relative, text in pairs(files) do
                    local file = path.join(folder, relative)
                    os.mkdir(path.directory(file))
                    io.writefile(file, text)
                end
            end

            -- a program that echoes its arguments and ends with the code the first one names
            local echo = path.join(root, "echo")
            writePackage(echo, 'package demo.echo {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'import @std.system;\n\nmain() -> int {\n'
                    .. '    string[] argv = system.arguments();\n'
                    .. '    for (long i = 1; i < argv.length; i++) {\n'
                    .. '        print("argument {i}: {argv[i]}");\n    }\n'
                    .. '    if (argv.length > 1) {\n'
                    .. '        return parsed(argv[1]);\n    }\n    return 0;\n}\n\n'
                    .. 'parsed(string text) -> int {\n'
                    .. '    if (text == "7") {\n        return 7;\n    }\n    return 0;\n}\n',
            })

            run({"run", echo}, nil, 0, "!argument 1")
            assertTempEmpty("ens run")
            run({"run", echo, "--", "one", "two three"}, nil, 0,
                "argument 1: one", "argument 2: two three")
            assertTempEmpty("ens run with arguments")
            run({"run", echo, "--", "7"}, nil, 7, "argument 1: 7")
            run({"run", path.join(tests_dir, "hello.ens")}, nil, 0, "Hello, world!")
            run({"run", echo, "-v"}, nil, 0, "building echo for", "running")

            -- a target with no main() is a usage problem, not a build failure
            run({"run", path.join(tests_dir, "pkg_import_dep")}, nil, 2, "is not a program to run",
                "does not define main()")

            -- a program that does not compile fails the work rather than the command line
            local broken = path.join(root, "broken")
            writePackage(broken, 'package demo.broken {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'main() -> int {\n    return missing();\n}\n',
            })
            run({"run", broken}, nil, 1, "did not compile, so it did not run")
            assertTempEmpty("a failed ens run")

            -- '--target' asks for a machine this command cannot run what it builds on
            run({"run", echo, "--target", "aarch64-unknown-linux-gnu"}, nil, 2,
                "runs what it builds", "drop '--target'")

            -- a workspace root runs its one program, and says so when there is none or several
            local one = path.join(root, "one")
            os.mkdir(one)
            io.writefile(path.join(one, "ens.package"),
                'workspace {\n    member "app";\n    member "lib";\n}\n')
            writePackage(path.join(one, "app"), 'package one.app {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'main() -> int {\n    print("the one program");\n    return 0;\n}\n',
            })
            writePackage(path.join(one, "lib"), 'package one.lib {\n    ens "0.1";\n}\n', {
                ["src/greet.ens"] = 'export greet() -> string {\n    return "hi";\n}\n',
            })
            run({"run", one}, nil, 0, "the one program")

            local none = path.join(root, "none")
            os.mkdir(none)
            io.writefile(path.join(none, "ens.package"), 'workspace {\n    member "lib";\n}\n')
            writePackage(path.join(none, "lib"), 'package none.lib {\n    ens "0.1";\n}\n', {
                ["src/greet.ens"] = 'export greet() -> string {\n    return "hi";\n}\n',
            })
            run({"run", none}, nil, 2, "holds no program to run")

            -- a workspace root whose program depends on a sibling member: the sibling's text is
            -- what comes out, everything after '--' reaches the program, the program's own code
            -- comes back, and the folder the command was started from is left as it was.
            local from = path.join(root, "from")
            os.mkdir(from)
            run({"run", path.join(tests_dir, "cli_workspace"), "--", "alpha", "beta"},
                {curdir = from}, 2, "cli workspace greeting", "alpha", "beta")
            assertEmptyFolder(from, "folder it was started from", "ens run at a workspace root")
            assertTempEmpty("ens run at a workspace root")

            local several = path.join(root, "several")
            os.mkdir(several)
            io.writefile(path.join(several, "ens.package"),
                'workspace {\n    member "first";\n    member "second";\n}\n')
            for _, member in ipairs({"first", "second"}) do
                writePackage(path.join(several, member),
                    'package several.' .. member .. ' {\n    ens "0.1";\n}\n', {
                    ["src/main.ens"] = 'main() -> int {\n    return 0;\n}\n',
                })
            end
            run({"run", several}, nil, 2, "more than one program to run", "'several.first'",
                "'several.second'")

            -- a package whose tests pass, fail, and are narrowed by a filter
            local suite = path.join(root, "suite")
            writePackage(suite, 'package demo.suite {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export twice(long n) -> long {\n    return n * 2;\n}\n',
                ["tests/math_test.ens"] = 'import @std.testing;\nimport math;\n\n'
                    .. 'test "twice a small number" {\n'
                    .. '    try testing.assertEqual(math.twice(2), 4L);\n}\n\n'
                    .. 'test "twice zero" {\n'
                    .. '    try testing.assertEqual(math.twice(0), 0L);\n}\n',
            })
            -- one target has nothing to tell apart, so it is neither announced nor added up
            run({"test", suite}, nil, 0, "PASS twice a small number", "PASS twice zero",
                "2/2 tests passed", "!demo.suite:", "!tests passed across")
            assertTempEmpty("ens test")
            run({"test", suite, "--filter", "zero"}, nil, 0, "PASS twice zero",
                "1/1 tests passed", "!twice a small")
            run({"test", suite, "--filter", "unicorn"}, nil, 0, "has 'unicorn' in its description",
                "2 tests are there in all")
            assertTempEmpty("a filter that matched nothing")

            local failing = path.join(root, "failing")
            writePackage(failing, 'package demo.failing {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export twice(long n) -> long {\n    return n * 3;\n}\n',
                ["tests/math_test.ens"] = 'import @std.testing;\nimport math;\n\n'
                    .. 'test "twice a small number" {\n'
                    .. '    try testing.assertEqual(math.twice(2), 4L);\n}\n',
            })
            run({"test", failing}, nil, 1, "FAIL twice a small number", "0/1 tests passed")

            -- two test files of the same name would be imported as one module
            local collide = path.join(root, "collide")
            writePackage(collide, 'package demo.collide {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export one() -> long {\n    return 1;\n}\n',
                ["tests/lex/scan_test.ens"] = 'test "lexing" {\n}\n',
                ["tests/parse/scan_test.ens"] = 'test "parsing" {\n}\n',
            })
            run({"test", collide}, nil, 2, "lex/scan_test.ens", "parse/scan_test.ens",
                "'scan_test'")

            -- only the runner may be the program's entry point
            local entry = path.join(root, "entry")
            writePackage(entry, 'package demo.entry {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export one() -> long {\n    return 1;\n}\n',
                ["tests/math_test.ens"] = 'test "counting" {\n}\n\n'
                    .. 'main() -> int {\n    return 0;\n}\n',
            })
            run({"test", entry}, nil, 2, "math_test.ens", "defines main()")

            -- a folder holding no tests, and a '--tests' folder that is not there
            local bare = path.join(root, "bare")
            writePackage(bare, 'package demo.bare {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export one() -> long {\n    return 1;\n}\n',
            })
            run({"test", bare}, nil, 0, "there are no tests in", "_test.ens")
            run({"test", suite, "--tests", path.join(root, "nowhere")}, nil, 2, "is not a folder")

            -- '--tests' names where the tests of one target live
            run({"test", suite, "--tests", path.join(suite, "tests")}, nil, 0, "2/2 tests passed")

            -- a file is not a folder of tests, and a workspace root tests its members
            run({"test", path.join(tests_dir, "hello.ens")}, nil, 2, "is one file",
                "folder the tests live in")
            run({"test", one, "--tests", path.join(suite, "tests")}, nil, 2,
                "'--tests' names one folder of tests")
            run({"test", one}, nil, 0, "there are no tests in")

            -- a workspace root tests every member: each member's results arrive under its own name,
            -- and the run ends by saying what the whole workspace came to
            local suites = path.join(root, "suites")
            os.mkdir(suites)
            io.writefile(path.join(suites, "ens.package"),
                'workspace {\n    member "alpha";\n    member "beta";\n}\n')
            writePackage(path.join(suites, "alpha"), 'package suite.alpha {\n    ens "0.1";\n}\n', {
                ["src/thing.ens"] = 'export tag() -> string {\n    return "alpha";\n}\n',
                ["tests/thing_test.ens"] = 'import @std.testing;\nimport thing;\n\n'
                    .. 'test "alpha holds" {\n'
                    .. '    try testing.assertEqual(thing.tag(), "alpha");\n}\n',
            })
            local beta_tests = path.join(suites, "beta", "tests", "thing_test.ens")
            writePackage(path.join(suites, "beta"), 'package suite.beta {\n    ens "0.1";\n}\n', {
                ["src/thing.ens"] = 'export tag() -> string {\n    return "beta";\n}\n',
                ["tests/thing_test.ens"] = 'import @std.testing;\nimport thing;\n\n'
                    .. 'test "beta holds" {\n'
                    .. '    try testing.assertEqual(thing.tag(), "beta");\n}\n\n'
                    .. 'test "beta counts" {\n    try testing.assertEqual(1, 1);\n}\n',
            })
            run({"test", suites}, nil, 0, "[1/2] suite.alpha:", "PASS alpha holds",
                "[2/2] suite.beta:", "PASS beta counts", "3/3 tests passed across 2 members")
            assertTempEmpty("ens test over a workspace")

            -- one member's failures neither stop the other members nor go missing from the total
            io.writefile(beta_tests, 'import @std.testing;\nimport thing;\n\n'
                .. 'test "beta holds" {\n'
                .. '    try testing.assertEqual(thing.tag(), "wrong");\n}\n')
            run({"test", suites}, nil, 1, "PASS alpha holds", "FAIL beta holds",
                "1/2 tests passed across 2 members", "!did not finish")

            -- a member whose tests never ran is named, and the total is short by exactly what that
            -- member had rather than pretending those tests were never there
            io.writefile(beta_tests, 'import @std.testing;\n\n'
                .. 'test "beta broken" {\n    try testing.assertEqual(nope(), 1);\n}\n')
            run({"test", suites}, nil, 2, "the tests did not compile",
                "1/2 tests passed across 2 members; 'suite.beta' did not finish")
            assertTempEmpty("a workspace member whose tests did not compile")

            -- a member with no tests of its own is not one of the members the total counts
            os.rm(beta_tests)
            run({"test", suites}, nil, 0, "there are no tests in",
                "1/1 tests passed across 1 member")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- `ens override`: the three forms driven against a scratch workspace, with every edit
        -- checked byte for byte, and a build proving the redirection reaches every member of the
        -- workspace it was written beside.
        local function run_cli_overriding(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "ensoverride")
            os.tryrm(root)
            local ws_dir = path.join(root, "ws")
            os.mkdir(path.join(ws_dir, "app", "src"))
            os.mkdir(path.join(root, "json", "src"))
            os.mkdir(path.join(root, "wrong"))
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end
            env.ENS_STDLIB = path.join(os.projectdir(), "libs")

            io.writefile(path.join(ws_dir, "ens.package"),
                'workspace {\n    member "app";\n    member "tool";\n}\n')
            io.writefile(path.join(ws_dir, "app", "ens.package"),
                'package demo.app {\n    ens "0.1";\n\n    dependency acme.json "1.0";\n}\n')
            io.writefile(path.join(ws_dir, "app", "src", "main.ens"),
                'import @acme.json.parse;\n\nmain() -> int {\n    print(parse.tag());\n'
                .. '    return 0;\n}\n')
            -- a second member depending on the same package, so the notice a build prints about the
            -- override is asserted to be one notice rather than one per member
            os.mkdir(path.join(ws_dir, "tool", "src"))
            io.writefile(path.join(ws_dir, "tool", "ens.package"),
                'package demo.tool {\n    ens "0.1";\n\n'
                .. '    dependency acme.json "1.0";\n}\n')
            io.writefile(path.join(ws_dir, "tool", "src", "helper.ens"),
                'import @acme.json.parse;\n\nexport describe() -> string {\n'
                .. '    return parse.tag();\n}\n')
            io.writefile(path.join(root, "json", "ens.package"),
                'package acme.json {\n    ens "0.1";\n}\n')
            io.writefile(path.join(root, "json", "src", "parse.ens"),
                'export tag() -> string {\n    return "json override";\n}\n')
            io.writefile(path.join(root, "wrong", "ens.package"),
                'package acme.other {\n    ens "0.1";\n}\n')

            local overrides_file = path.join(ws_dir, "ens.overrides")
            local in_ws = {curdir = ws_dir, envs = env}

            -- run the command and assert its exit code and the fragments its output must carry; a
            -- fragment prefixed '!' must not appear.
            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(host_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("ens %s: output must not carry "
                                .. "%q\n%s", label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("ens %s: output missing %q\n%s",
                            label, fragment, out))
                    end
                end
                return rc, out
            end

            -- the whole file, so an edit that changed anything but the declaration it was about
            -- fails here.
            local function expect_file(label, expected)
                local actual = (io.readfile(overrides_file) or ""):gsub("\r\n", "\n")
                if actual ~= expected then
                    table.insert(failures, string.format("%s: ens.overrides is %q, expected %q",
                        label, actual, expected))
                end
            end

            -- with nothing redirected, the dependency has no source at all
            run({"override", "list"}, in_ws, 0, "No packages are overridden")
            run({"build"}, in_ws, 1, "No source for package 'acme.json'")

            -- add writes the file, list says the folder works, and the build resolves through it
            run({"override", "add", "acme.json", "../json"}, in_ws, 0,
                "Added the override for package 'acme.json': ../json")
            expect_file("add", 'overrides {\n    override acme.json "../json";\n}\n')
            run({"override", "list"}, in_ws, 0, "acme.json -> ../json", "!not usable")
            -- the build says what it is taking from the override, because nothing in the program
            -- does, and a check says it too
            local notice = "Using the override for package 'acme.json': "
                .. path.absolute(path.join(root, "json")):gsub("\\", "/")
            run({"build"}, in_ws, 0, "demo.app: built", notice)
            run({"check"}, in_ws, 0, notice)

            -- it survives '--quiet' and rides the error stream beside the problems: quiet hides an
            -- account of the work, not a departure from what the manifests declare
            local quiet_out = path.join(out_dir, name .. ".quiet.out")
            local quiet_err = path.join(out_dir, name .. ".quiet.err")
            local quiet_rc = execSplit(host_exe, {"build", "--quiet"}, quiet_out, quiet_err, in_ws)
            local quiet_stdout = captured(quiet_out)
            local quiet_stderr = captured(quiet_err)
            if quiet_rc ~= 0 or quiet_stdout ~= "" or not quiet_stderr:find(notice, 1, true) then
                table.insert(failures, string.format("ens build --quiet: exit=%s stdout=%q "
                    .. "stderr=%q", tostring(quiet_rc), quiet_stdout, quiet_stderr))
            end

            -- both members take the package from the same override, and the build says so once
            local _, said = run({"build"}, in_ws, 0, "demo.app: built", "demo.tool: compiled")
            local times = 0
            for _ in said:gmatch("Using the override for package 'acme%.json'") do
                times = times + 1
            end
            if times ~= 1 then
                table.insert(failures, string.format("the override notice appeared %d times, "
                    .. "expected once:\n%s", times, said))
            end
            local built = member_artifact(ws_dir, "app")
            if os.isfile(built) then
                local rc = execMerged(built, {}, log, {envs = env})
                local out = captured(log)
                if rc ~= 0 or out ~= "json override" then
                    table.insert(failures, string.format("the built app: exit=%s stdout=%q",
                        tostring(rc), out))
                end
            else
                table.insert(failures, string.format("expected executable %s", built))
            end

            -- with no path at all the command works on the workspace above the folder it was run
            -- in, and what runs is the code the override points at
            run({"run"}, in_ws, 0, "json override")

            -- a folder declaring another package is refused, naming what it found, and the file is
            -- left as it was
            run({"override", "add", "acme.json", "../wrong"}, in_ws, 1,
                "declares package 'acme.other' instead")
            expect_file("refused", 'overrides {\n    override acme.json "../json";\n}\n')

            -- edits are targeted: comments, blank lines and other declarations survive byte-exact
            io.writefile(overrides_file, '// local checkouts\noverrides {\n'
                .. '    override acme.json "../wrong";  // wrong on purpose\n'
                .. '\n'
                .. '    override beta.tools "../missing";\n}\n')
            run({"override", "add", "acme.json", "../json"}, in_ws, 0,
                "Replaced the override for package 'acme.json': now ../json")
            expect_file("replace", '// local checkouts\noverrides {\n'
                .. '    override acme.json "../json";  // wrong on purpose\n'
                .. '\n'
                .. '    override beta.tools "../missing";\n}\n')

            -- list reports a valid target and an invalid one, with the reason for the invalid one
            local _, listed = run({"override", "list"}, in_ws, 0, "acme.json -> ../json",
                "beta.tools -> ../missing", "not usable", "there is no ens.package manifest at")
            if listed:find("acme.json -> ../json (", 1, true) then
                table.insert(failures, string.format("a usable override was reported with a "
                    .. "reason:\n%s", listed))
            end

            run({"override", "remove", "beta.tools"}, in_ws, 0,
                "Removed the override for package 'beta.tools'")
            expect_file("remove", '// local checkouts\noverrides {\n'
                .. '    override acme.json "../json";  // wrong on purpose\n'
                .. '\n}\n')

            -- a folder is read from where the command ran and written down relative to the
            -- workspace, so running from a subfolder still records one path
            os.mkdir(path.join(ws_dir, "notes"))
            run({"override", "add", "acme.json", "../../json"},
                {curdir = path.join(ws_dir, "notes"), envs = env}, 0, "now ../json")
            expect_file("from a subfolder", '// local checkouts\noverrides {\n'
                .. '    override acme.json "../json";  // wrong on purpose\n'
                .. '\n}\n')

            -- state and usage problems, each one saying what to do instead
            run({"override", "remove", "nope.pkg"}, in_ws, 1,
                "package 'nope.pkg' is not overridden", "ens override list")
            run({"override", "add", "not/a/name!", "../json"}, in_ws, 2, "is not a package name")
            run({"override", "add", "acme.json"}, in_ws, 2, "ens override add")
            run({"override", "wat"}, in_ws, 2, "unknown command 'wat'")
            run({"override"}, in_ws, 2, "ens override")
            run({"override", "list"}, {curdir = root, envs = env}, 2,
                "no ens.package manifest was found")

            -- a file that is not an overrides file is reported rather than overwritten
            local plain = path.join(root, "plain")
            os.mkdir(plain)
            io.writefile(path.join(plain, "ens.package"), 'package demo.plain {\n    ens "0.1";\n}\n')
            io.writefile(path.join(plain, "ens.overrides"), 'workspace {\n    member "x";\n}\n')
            run({"override", "list"}, {curdir = plain, envs = env}, 1,
                "does not hold an overrides declaration")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- git-sourced dependencies, driven end to end against scratch repositories over file://
        -- URLs and a scratch cache, so nothing touches the network or this machine's own cache.
        local function run_cli_dependencies(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "ensdeps")
            os.tryrm(root)
            os.tryrm(root .. ".away")
            os.mkdir(root)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end
            env.ENS_STDLIB = path.join(os.projectdir(), "libs")

            local repos = path.join(root, "repos")
            local cache = path.join(root, "cache")
            local cold = path.join(root, "cold")

            local function withCache(cache_dir)
                local envs = {}
                for key, value in pairs(env) do envs[key] = value end
                envs.ENS_CACHE = cache_dir
                return envs
            end

            -- run the command and assert its exit code and the fragments its output must carry; a
            -- fragment prefixed '!' must not appear.
            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(host_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("ens %s: output must not carry "
                                .. "%q\n%s", label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
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
                local rc = execMerged(exe, {}, log, {envs = env})
                local out = captured(log)
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            -- a scratch repository keeps its metadata in a sibling folder rather than a '.git'
            -- child, so nothing under build/ looks like a checkout to an editor. The file:// URLs
            -- point straight at the metadata folder.
            local function gitdir_of(dir)
                return path.absolute(dir) .. ".gitdir"
            end

            local function git(dir, ...)
                os.iorunv("git", table.join({"--git-dir", gitdir_of(dir), "--work-tree",
                    path.absolute(dir), "-c", "user.name=ens", "-c", "user.email=ens@test",
                    "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
                    "-c", "core.autocrlf=false"}, {...}), {curdir = dir})
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

            -- the repositories: a package with three tagged versions (one behind an annotated 'v'
            -- tag, one spelled both ways), a package raising another's version transitively, a
            -- workspace-form tag root, a submodule user, and one that gets re-tagged under the
            -- build's feet.
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
            git(json_dir, "tag", "-a", "-m", "release 1.1", "v1.1")
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

            -- the application: three git dependencies, one of which raises another's version
            -- transitively, so the build uses the highest version required of it.
            local app = path.join(root, "app")
            local function write_app(json_version)
                write_package(app, "demo.gitapp", {
                    'dependency alex.json "' .. json_version .. '" from "' .. url_json .. '";',
                    'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                    'dependency beta.utils "1.0" from "' .. url_utils .. '";',
                })
            end
            os.mkdir(path.join(app, "src"))
            write_app("1.0")
            io.writefile(path.join(app, "src", "main.ens"),
                'import @alex.json.parse;\nimport @acme.tools.tool;\n\n'
                .. 'main() -> int {\n    print(parse.tag() + " | " + tool.describe());\n'
                .. '    return 0;\n}\n')
            local in_app = {curdir = app, envs = withCache(cache)}
            local lock_file = path.join(app, "ens.lock")
            local function lock_text()
                return ((io.readfile(lock_file) or ""):gsub("\r\n", "\n"))
            end

            -- the first build fetches, locks, links, and the program runs against the
            -- transitively raised beta.utils 1.1. The tag listing is read, never shown.
            run({"build", "."}, in_app, 0,
                "Fetched alex.json 1.0 from " .. url_json .. " (tag 1.0)",
                "Fetched acme.tools 1.0",
                "Fetched beta.utils 1.1",
                "Updated ens.lock: locked acme.tools 1.0, locked alex.json 1.0, "
                    .. "locked beta.utils 1.1",
                "!refs/tags/")
            run_program(path.join(app, "gitapp" .. exe_suffix), 0, "json 1.0 | tools(utils 1.1)")

            -- the lock is complete, sorted, and records what each package requires
            local locked = lock_text()
            local head = "lock 1\nroot demo.gitapp\npackage acme.tools 1.0\n"
                .. "source " .. url_tools .. " " .. commit_of(tools_dir, "1.0") .. "\n"
            if locked:sub(1, #head) ~= head then
                table.insert(failures, string.format("the lock starts %q, expected %q",
                    locked:sub(1, #head), head))
            end
            for _, fragment in ipairs({
                "\nrequire beta.utils 1.1\n",
                "\npackage alex.json 1.0\nsource " .. url_json .. " "
                    .. commit_of(json_dir, "1.0") .. "\ncontent sha256:",
                "\npackage beta.utils 1.1\nsource " .. url_utils .. " "
                    .. commit_of(utils_dir, "1.1") .. "\ncontent sha256:",
            }) do
                if not locked:find(fragment, 1, true) then
                    table.insert(failures, string.format("ens.lock missing %q\n%s", fragment,
                        locked))
                end
            end

            -- the fetched files sit in the cache under the digest the lock records
            local json_hex = locked:match("package alex%.json 1%.0\nsource [^\n]*\n"
                .. "content sha256:(%x+)")
            if not json_hex then
                table.insert(failures, "the lock records no content digest for alex.json")
            elseif not os.isdir(path.join(cache, "trees", "sha256-" .. json_hex)) then
                table.insert(failures, "the cache holds no tree under alex.json's digest")
            end

            -- a locked build asks the network nothing: the repositories are gone and it still
            -- reproduces, with the lock untouched
            os.mv(repos, repos .. ".away")
            run({"build", "."}, in_app, 0, "gitapp: built", "!Fetched", "!Updated ens.lock")
            if lock_text() ~= locked then
                table.insert(failures, "a build that fetched nothing rewrote ens.lock")
            end
            run({"build", ".", "--offline"}, in_app, 0, "gitapp: built", "!Fetched")
            run({"check", ".", "--offline", "--locked"}, in_app, 0, "nothing to report")

            -- a cold cache under --offline fails by name, and says how to fix it
            run({"build", ".", "--offline"}, {curdir = app, envs = withCache(cold)}, 1,
                "'--offline' forbids fetching package", "the cache does not hold it",
                "without '--offline'")
            os.mv(repos .. ".away", repos)

            -- --locked turns a pending change into an error, names what would change, and leaves
            -- the lock exactly as it was
            write_app("1.1")
            run({"build", ".", "--locked"}, in_app, 1,
                "ens.lock no longer matches what this build requires",
                "updated alex.json 1.0 -> 1.1", "'--locked' does not allow it to change")
            if lock_text() ~= locked then
                table.insert(failures, "--locked changed ens.lock")
            end

            -- the update fetches the raised version through its annotated 'v' tag and says what
            -- changed and nothing more
            run({"build", "."}, in_app, 0,
                "Fetched alex.json 1.1 from " .. url_json .. " (tag v1.1)",
                "Updated ens.lock: updated alex.json 1.0 -> 1.1", "!locked beta.utils")
            run_program(path.join(app, "gitapp" .. exe_suffix), 0, "json 1.1 | tools(utils 1.1)")
            if not lock_text():find("package alex.json 1.1", 1, true) then
                table.insert(failures, "ens.lock does not record the raised version")
            end

            -- both '2.0' and 'v2.0' exist, so the version could mean either
            write_app("2.0")
            run({"build", "."}, in_app, 1,
                "both the tags '2.0' and 'v2.0' exist at " .. url_json,
                "remove or rename one of the two tags")

            -- a version nothing is tagged for names both spellings and the way in for unreleased
            -- work
            write_app("9.9")
            run({"build", "."}, in_app, 1,
                "package 'alex.json' has no tag '9.9' or 'v9.9'",
                "ens override add alex.json <folder>")
            write_app("1.1")

            -- requirements spanning majors are refused, naming both requirers
            local spanning = path.join(root, "spanning")
            os.mkdir(path.join(spanning, "src"))
            write_package(spanning, "demo.major", {
                'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                'dependency beta.utils "2.0" from "' .. url_utils .. '";',
            })
            io.writefile(path.join(spanning, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = spanning, envs = withCache(cache)}, 1,
                "the requirements on package 'beta.utils' span major versions",
                'requires "1.1"', 'requires "2.0"',
                "one of the two requirements has to change")

            -- every package has to agree on where a dependency comes from
            local disagreeing = path.join(root, "disagreeing")
            os.mkdir(path.join(disagreeing, "src"))
            write_package(disagreeing, "demo.urls", {
                'dependency acme.tools "1.0" from "' .. url_tools .. '";',
                'dependency beta.utils "1.0" from "' .. url_json .. '";',
            })
            io.writefile(path.join(disagreeing, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = disagreeing, envs = withCache(cache)}, 1,
                "package 'beta.utils' is required from two different repositories",
                "one of the two 'from' clauses has to change")

            -- the tag has to declare the package that was asked for
            local misnamed = path.join(root, "misnamed")
            os.mkdir(path.join(misnamed, "src"))
            write_package(misnamed, "demo.wrongname",
                {'dependency wrong.name "1.0" from "' .. url_json .. '";'})
            io.writefile(path.join(misnamed, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = misnamed, envs = withCache(cache)}, 1,
                "declares package 'alex.json' instead", "names have to match exactly")

            -- a workspace-form tag root gives up the member that declares the package, the members
            -- resolve each other inside the fetched tree, and the sibling is never locked
            local using_ws = path.join(root, "usingws")
            os.mkdir(path.join(using_ws, "src"))
            write_package(using_ws, "demo.wsapp",
                {'dependency ws.core "1.0" from "' .. url_ws .. '";'})
            io.writefile(path.join(using_ws, "src", "main.ens"),
                'import @ws.core.api;\n\nmain() -> int {\n    print(api.describe());\n'
                .. '    return 0;\n}\n')
            run({"build", "."}, {curdir = using_ws, envs = withCache(cache)}, 0,
                "Fetched ws.core 1.0")
            run_program(path.join(using_ws, "wsapp" .. exe_suffix), 0, "core(extra)")
            local ws_lock = (io.readfile(path.join(using_ws, "ens.lock")) or "")
            if not ws_lock:find("package ws.core 1.0", 1, true) then
                table.insert(failures, "the workspace-form package was not locked")
            end
            if ws_lock:find("ws.extra", 1, true) then
                table.insert(failures, "a member internal to the fetched workspace was locked")
            end

            -- a workspace-form tag root that declares nothing asked for says what it does declare
            local missing_member = path.join(root, "missingmember")
            os.mkdir(path.join(missing_member, "src"))
            write_package(missing_member, "demo.nomember",
                {'dependency ws.absent "1.0" from "' .. url_ws .. '";'})
            io.writefile(path.join(missing_member, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = missing_member, envs = withCache(cache)}, 1,
                "none of its members declares package 'ws.absent'", "'ws.core' and 'ws.extra'")

            -- a package using submodules is refused, because a package is used as it was fetched
            local using_sub = path.join(root, "usingsub")
            os.mkdir(path.join(using_sub, "src"))
            write_package(using_sub, "demo.subapp",
                {'dependency sub.pkg "1.0" from "' .. url_sub .. '";'})
            io.writefile(path.join(using_sub, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = using_sub, envs = withCache(cache)}, 1,
                "uses git submodules", "has to be self-contained",
                "declare what the submodule holds as a dependency")

            -- a tag that moved is caught against the lock, naming both digests
            local using_retag = path.join(root, "usingretag")
            os.mkdir(path.join(using_retag, "src"))
            write_package(using_retag, "demo.retag",
                {'dependency rt.pkg "1.0" from "' .. url_retag .. '";'})
            io.writefile(path.join(using_retag, "src", "main.ens"),
                'import @rt.pkg.thing;\n\nmain() -> int {\n    print(thing.tag());\n'
                .. '    return 0;\n}\n')
            local in_retag = {curdir = using_retag, envs = withCache(cache)}
            run({"build", "."}, in_retag, 0, "Fetched rt.pkg 1.0")
            run_program(path.join(using_retag, "retag" .. exe_suffix), 0, "retag 1")
            io.writefile(path.join(retag_dir, "src", "thing.ens"),
                'export tag() -> string {\n    return "retag 2";\n}\n')
            commit_all(retag_dir, "moved")
            git(retag_dir, "tag", "-f", "1.0")
            os.tryrm(path.join(cache, "trees"))
            run({"build", "."}, in_retag, 1,
                "are not the files ens.lock records", "the lock has sha256:",
                "hashes to sha256:", "may have been moved", "delete ens.lock")

            -- losing the last git dependency removes the lock, and --locked refuses that too
            write_package(using_retag, "demo.retag")
            io.writefile(path.join(using_retag, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", ".", "--locked"}, in_retag, 1,
                "this build fetches nothing any more", "no prebuilt library",
                "'--locked' does not allow it to change")
            run({"build", "."}, in_retag, 0,
                "Removed ens.lock: this build fetches nothing any more")
            if os.isfile(path.join(using_retag, "ens.lock")) then
                table.insert(failures, "ens.lock survived losing its last git dependency")
            end

            -- a lock nobody may edit by hand says so rather than being read as one
            io.writefile(lock_file, "hand written\n")
            run({"build", "."}, in_app, 1, "never edited by hand", "delete it and build again")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- prebuilt native libraries, driven over file:// URLs and a scratch cache. The library is a
        -- valid empty static archive, so every platform's linker accepts it.
        local function run_cli_prebuilt(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "ensprebuilt")
            os.tryrm(root)
            os.mkdir(root)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end
            env.ENS_STDLIB = path.join(os.projectdir(), "libs")

            local cache = path.join(root, "cache")
            local cold = path.join(root, "cold")

            local function withCache(cache_dir)
                local envs = {}
                for key, value in pairs(env) do envs[key] = value end
                envs.ENS_CACHE = cache_dir
                return envs
            end

            local function run(argv, opt, expected_rc, ...)
                local rc = execMerged(host_exe, argv, log, opt)
                local out = io.readfile(log) or ""
                local label = table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("ens %s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("ens %s: output must not carry "
                                .. "%q\n%s", label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
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
                local rc = execMerged(exe, {}, log, {envs = env})
                local out = captured(log)
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            local files = path.join(root, "files")
            os.mkdir(files)
            local lib_file = path.join(files, "extras.lib")
            io.writefile(lib_file, "!<arch>\n", {encoding = "binary"})
            local good = "sha256:" .. hash.sha256(lib_file)
            local wrong = "sha256:" .. string.rep("0123456789abcdef", 4)
            local url_lib = "file:///" .. (path.absolute(lib_file):gsub("\\", "/"))

            local function binding(native_name, digest)
                return "    native " .. native_name .. " {\n"
                    .. '        windows artifact "' .. url_lib .. '" hash "' .. digest .. '";\n'
                    .. '        linux artifact "' .. url_lib .. '" hash "' .. digest .. '";\n'
                    .. '        macos artifact "' .. url_lib .. '" hash "' .. digest .. '";\n'
                    .. "    }\n"
            end

            -- the happy path: downloaded, checked, cached, and handed to the linker
            local app = path.join(root, "app")
            os.mkdir(path.join(app, "src"))
            io.writefile(path.join(app, "ens.package"),
                "package demo.prebuilt {\n" .. '    ens "0.1";\n\n'
                .. binding("extras", good) .. "}\n")
            io.writefile(path.join(app, "src", "main.ens"),
                'main() -> int {\n    print("prebuilt linked");\n    return 0;\n}\n')
            local in_app = {curdir = app, envs = withCache(cache)}
            run({"build", "."}, in_app, 0, "prebuilt: built")
            run_program(path.join(app, "prebuilt" .. exe_suffix), 0, "prebuilt linked")
            local stored = path.join(cache, "artifacts", good:gsub("^sha256:", ""), "extras.lib")
            if not os.isfile(stored) then
                table.insert(failures, "the downloaded library is not in the cache")
            end

            -- a project that fetches only a prebuilt library gets a lock too: the binary behind the
            -- URL is the one build input nobody can review by reading the repository
            local only_lock = path.join(app, "ens.lock")
            local locked_text = ((io.readfile(only_lock) or ""):gsub("\r\n", "\n"))
            local expected_lock = "lock 1\n"
                .. "root demo.prebuilt\n"
                .. "artifact extras linux " .. url_lib .. " " .. good .. "\n"
                .. "artifact extras macos " .. url_lib .. " " .. good .. "\n"
                .. "artifact extras windows " .. url_lib .. " " .. good .. "\n"
            if locked_text ~= expected_lock then
                table.insert(failures, string.format("an artifact-only build wrote %q, expected %q",
                    locked_text, expected_lock))
            end

            -- and it stays current: a second build changes nothing, and '--locked' is satisfied
            run({"build", ".", "--locked"}, in_app, 0, "prebuilt: built",
                "!ens.lock no longer matches")
            if ((io.readfile(only_lock) or ""):gsub("\r\n", "\n")) ~= locked_text then
                table.insert(failures, "a second artifact-only build rewrote ens.lock")
            end

            -- a cached library needs no network, even with the file it came from gone
            os.mv(files, files .. ".away")
            run({"build", ".", "--offline"}, in_app, 0, "prebuilt: built")
            run({"build", ".", "--offline"}, {curdir = app, envs = withCache(cold)}, 1,
                "'--offline' forbids downloading the prebuilt library for native 'extras'",
                "without '--offline'")
            os.mv(files .. ".away", files)

            -- rebinding and then unbinding a prebuilt library are both lock changes, so '--locked'
            -- refuses each of them by name and a build without it keeps the lock current
            local lifecycle = path.join(root, "lifecycle")
            os.mkdir(path.join(lifecycle, "src"))
            local function write_lifecycle(body)
                io.writefile(path.join(lifecycle, "ens.package"),
                    "package demo.lifecycle {\n" .. '    ens "0.1";\n\n' .. body .. "}\n")
            end
            io.writefile(path.join(lifecycle, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            local in_lifecycle = {curdir = lifecycle, envs = withCache(cache)}
            write_lifecycle(binding("extras", good))
            run({"build", "."}, in_lifecycle, 0,
                "Updated ens.lock: recorded the prebuilt libraries this build itself binds")
            write_lifecycle(binding("renamed", good))
            run({"build", ".", "--locked"}, in_lifecycle, 1,
                "ens.lock no longer matches what this build requires",
                "updated the prebuilt libraries this build itself binds",
                "'--locked' does not allow it to change")
            write_lifecycle("    native extras system;\n")
            run({"build", ".", "--locked"}, in_lifecycle, 1,
                "this build fetches nothing any more", "no prebuilt library",
                "'--locked' does not allow it to change")
            run({"build", "."}, in_lifecycle, 0,
                "Removed ens.lock: this build fetches nothing any more")
            if os.isfile(path.join(lifecycle, "ens.lock")) then
                table.insert(failures, "ens.lock survived losing its last prebuilt library")
            end

            -- a digest that does not match is refused, naming both digests, and nothing is cached
            local bad = path.join(root, "bad")
            os.mkdir(path.join(bad, "src"))
            io.writefile(path.join(bad, "ens.package"),
                "package demo.badhash {\n" .. '    ens "0.1";\n\n'
                .. binding("extras", wrong) .. "}\n")
            io.writefile(path.join(bad, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = bad, envs = withCache(cache)}, 1,
                "hashes to " .. good, "manifest declares " .. wrong, "will not be used",
                "put the new hash in the manifest")
            if os.isdir(path.join(cache, "artifacts", wrong:gsub("^sha256:", ""))) then
                table.insert(failures, "a library whose digest did not match was cached anyway")
            end

            -- a digest written in capitals is the same digest
            local capitals = path.join(root, "capitals")
            os.mkdir(path.join(capitals, "src"))
            io.writefile(path.join(capitals, "ens.package"),
                "package demo.capitals {\n" .. '    ens "0.1";\n\n'
                .. binding("extras", "sha256:" .. hash.sha256(lib_file):upper()) .. "}\n")
            io.writefile(path.join(capitals, "src", "main.ens"),
                'main() -> int {\n    print("capitals linked");\n    return 0;\n}\n')
            run({"build", "."}, {curdir = capitals, envs = withCache(cache)}, 0,
                "capitals: built")
            run_program(path.join(capitals, "capitals" .. exe_suffix), 0, "capitals linked")

            -- the lock records the bindings of the build's own package and of every fetched one,
            -- flattened per platform, so the exact native code a build links reads in one place
            local function gitdir_of(dir)
                return path.absolute(dir) .. ".gitdir"
            end
            local function git(dir, ...)
                os.iorunv("git", table.join({"--git-dir", gitdir_of(dir), "--work-tree",
                    path.absolute(dir), "-c", "user.name=ens", "-c", "user.email=ens@test",
                    "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
                    "-c", "core.autocrlf=false"}, {...}), {curdir = dir})
            end
            local dep_dir = path.join(root, "repos", "dep")
            os.mkdir(path.join(dep_dir, "src"))
            os.iorunv("git", {"init", "--bare", "-q", "-b", "main", gitdir_of(dep_dir)})
            io.writefile(path.join(dep_dir, "ens.package"),
                "package art.dep {\n" .. '    ens "0.1";\n\n'
                .. binding("depextras", good) .. "}\n")
            io.writefile(path.join(dep_dir, "src", "dep.ens"),
                'export tag() -> string {\n    return "dep with a prebuilt library";\n}\n')
            git(dep_dir, "add", "-A")
            git(dep_dir, "commit", "-q", "-m", "1.0")
            git(dep_dir, "tag", "1.0")
            local url_dep = "file:///" .. (gitdir_of(dep_dir):gsub("\\", "/"))

            local recorded = path.join(root, "recorded")
            os.mkdir(path.join(recorded, "src"))
            io.writefile(path.join(recorded, "ens.package"),
                "package demo.lockapp {\n" .. '    ens "0.1";\n\n'
                .. '    dependency art.dep "1.0" from "' .. url_dep .. '";\n\n'
                .. binding("extras", good) .. "}\n")
            io.writefile(path.join(recorded, "src", "main.ens"),
                'import @art.dep.dep;\n\nmain() -> int {\n    print(dep.tag());\n'
                .. '    return 0;\n}\n')
            run({"build", "."}, {curdir = recorded, envs = withCache(cache)}, 0,
                "Fetched art.dep 1.0", "Updated ens.lock: locked art.dep 1.0")
            run_program(path.join(recorded, "lockapp" .. exe_suffix), 0, "dep with a prebuilt library")
            local lock = ((io.readfile(path.join(recorded, "ens.lock")) or ""):gsub("\r\n", "\n"))
            local own = "root demo.lockapp\n"
                .. "artifact extras linux " .. url_lib .. " " .. good .. "\n"
                .. "artifact extras macos " .. url_lib .. " " .. good .. "\n"
                .. "artifact extras windows " .. url_lib .. " " .. good .. "\n"
            if not lock:find(own, 1, true) then
                table.insert(failures, string.format(
                    "ens.lock is missing the build's own prebuilt lines:\n%s", lock))
            end
            local theirs = "artifact depextras linux " .. url_lib .. " " .. good
                .. "\nartifact depextras macos " .. url_lib .. " " .. good
                .. "\nartifact depextras windows " .. url_lib .. " " .. good .. "\n"
            if not (lock:find("package art.dep 1.0\n", 1, true)
                    and lock:find(theirs, 1, true)) then
                table.insert(failures, string.format(
                    "ens.lock is missing the fetched package's prebuilt lines:\n%s", lock))
            end

            -- a workspace declares no package, so its members' bindings are recorded under their own
            -- names: the shape this repository itself uses has to be reviewable too
            local ws = path.join(root, "ws")
            os.mkdir(path.join(ws, "member", "src"))
            io.writefile(path.join(ws, "ens.package"),
                'workspace {\n    member "member";\n}\n')
            io.writefile(path.join(ws, "member", "ens.package"),
                "package demo.member {\n" .. '    ens "0.1";\n\n'
                .. '    dependency art.dep "1.0" from "' .. url_dep .. '";\n\n'
                .. binding("memberextras", good) .. "}\n")
            io.writefile(path.join(ws, "member", "src", "main.ens"),
                'import @art.dep.dep;\n\nmain() -> int {\n    print(dep.tag());\n'
                .. '    return 0;\n}\n')
            run({"build", "."}, {curdir = ws, envs = withCache(cache)}, 0, "demo.member: built")
            run_program(member_artifact(ws, "member"), 0, "dep with a prebuilt library")
            local ws_lock = ((io.readfile(path.join(ws, "ens.lock")) or ""):gsub("\r\n", "\n"))
            local recorded_member = "member demo.member\n"
                .. "artifact memberextras linux " .. url_lib .. " " .. good .. "\n"
                .. "artifact memberextras macos " .. url_lib .. " " .. good .. "\n"
                .. "artifact memberextras windows " .. url_lib .. " " .. good .. "\n"
            if not ws_lock:find(recorded_member, 1, true) then
                table.insert(failures, string.format(
                    "ens.lock is missing the member's prebuilt lines:\n%s", ws_lock))
            end
            -- a workspace has no root package to name, and the lock stays current across builds
            if ws_lock:find("\nroot ", 1, true) then
                table.insert(failures, string.format(
                    "a workspace's lock named a root package:\n%s", ws_lock))
            end
            run({"build", ".", "--locked"}, {curdir = ws, envs = withCache(cache)}, 0,
                "demo.member: built", "!ens.lock no longer matches")
            if ((io.readfile(path.join(ws, "ens.lock")) or ""):gsub("\r\n", "\n")) ~= ws_lock then
                table.insert(failures, "a second build rewrote the member's lock")
            end

            -- changing a member's binding is a lock change '--locked' has to refuse by name
            io.writefile(path.join(ws, "member", "ens.package"),
                "package demo.member {\n" .. '    ens "0.1";\n\n'
                .. '    dependency art.dep "1.0" from "' .. url_dep .. '";\n\n'
                .. binding("renamedextras", good) .. "}\n")
            run({"build", ".", "--locked"}, {curdir = ws, envs = withCache(cache)}, 1,
                "ens.lock no longer matches what this build requires",
                "updated the prebuilt libraries demo.member binds",
                "'--locked' does not allow it to change")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- version delegation: `ens` handing a whole invocation to another toolchain. A copy of the
        -- built command is installed as toolchain 9.9 in a scratch folder, which is what makes the
        -- mechanism observable while only one real version exists - the copy answers 'ens 0.1', so
        -- it would hand the work straight back if the loop guard let it.
        local function run_cli_toolchain(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "enstoolchain")
            os.tryrm(root)
            os.mkdir(root)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env, native_libraries, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM library",
                    full = string.format("%s: %s", name, env_error)}
            end

            local toolchains = path.join(root, "toolchains")
            local installed_dir = path.join(toolchains, "9.9")
            local installed_exe = path.join(installed_dir, "ens" .. exe_suffix)
            os.mkdir(installed_dir)
            os.cp(host_exe, installed_exe)
            -- the copy loads LLVM and the linker bridge at run time, and on Windows the loader
            -- looks beside the executable first.
            placeNativeLibraries(native_libraries, installed_dir)
            local empty = path.join(root, "empty")
            os.mkdir(empty)

            -- run a command and assert its exit code and the fragments its output must carry; a
            -- fragment prefixed '!' must not appear. `vars` are the environment variables this one
            -- run adds, and ENS_TOOLCHAIN is cleared first so a variable set in the shell running
            -- the suite cannot decide the answer.
            local function run(program, argv, vars, expected_rc, ...)
                local envs = {}
                for key, value in pairs(env) do envs[key] = value end
                envs.ENS_TOOLCHAIN = nil
                for key, value in pairs(vars or {}) do envs[key] = value end
                local rc = execMerged(program, argv, log, {envs = envs})
                local out = io.readfile(log) or ""
                local label = path.filename(program) .. " " .. table.concat(argv, " ")
                if expected_rc ~= nil and rc ~= expected_rc then
                    table.insert(failures, string.format("%s: exit=%s expected=%s\n%s",
                        label, tostring(rc), tostring(expected_rc), out))
                end
                for _, fragment in ipairs({...}) do
                    if fragment:sub(1, 1) == "!" then
                        if out:find(fragment:sub(2), 1, true) then
                            table.insert(failures, string.format("%s: output must not carry %q\n%s",
                                label, fragment:sub(2), out))
                        end
                    elseif not out:find(fragment, 1, true) then
                        table.insert(failures, string.format("%s: output missing %q\n%s",
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
                local out = captured(log)
                if rc ~= expected_rc or out ~= expected_stdout then
                    table.insert(failures, string.format("%s: exit=%s stdout=%q",
                        path.filename(exe), tostring(rc), out))
                end
            end

            -- the fixtures: a package written for 9.9 in a folder whose name holds a space, so a hop
            -- that mangled the command line could not build it; a library written for 9.9, whose
            -- refusal of --output is a usage exit code to propagate; and one that does not compile,
            -- whose failure is the other exit code.
            local pkg = path.join(root, "spaced pkg")
            os.mkdir(path.join(pkg, "src"))
            io.writefile(path.join(pkg, "ens.package"),
                'package demo.spaced {\n    ens "9.9";\n}\n')
            io.writefile(path.join(pkg, "src", "main.ens"),
                'main() -> int {\n    print("built by a delegate");\n    return 0;\n}\n')
            local library = path.join(root, "library")
            os.mkdir(path.join(library, "src"))
            io.writefile(path.join(library, "ens.package"),
                'package demo.library {\n    ens "9.9";\n}\n')
            io.writefile(path.join(library, "src", "greet.ens"),
                'export greet() -> string {\n    return "hi";\n}\n')
            local broken = path.join(root, "broken")
            os.mkdir(path.join(broken, "src"))
            io.writefile(path.join(broken, "ens.package"),
                'package demo.broken {\n    ens "9.9";\n}\n')
            io.writefile(path.join(broken, "src", "main.ens"),
                'main() -> int {\n    return missing();\n}\n')

            local hello = path.join(tests_dir, "hello.ens")
            local chains = {ENS_TOOLCHAINS = toolchains}
            local nothing_installed = {ENS_TOOLCHAINS = empty}
            -- every path the command prints is written with '/', whatever this machine writes
            local function slashed(p) return (p:gsub("\\", "/")) end
            -- the hop always names the program it hands the work to, so that name appearing is the
            -- hop having happened and its absence is the work having stayed here
            local hopped_to = slashed(installed_exe)
            local not_hopped = "!" .. hopped_to

            -- the hop: the package is built by the toolchain installed as the version it is written
            -- for, and the command line reaches it exactly as it was written here, spaces and all
            local spaced_out = path.join(root, "spaced out.exe")
            run(host_exe, {"build", pkg, "--output", spaced_out, "-v"}, chains, 0,
                "written for Ens 9.9", hopped_to, "built")
            run_program(spaced_out, 0, "built by a delegate")

            -- the exit code of the toolchain that did the work is this command's own
            run(host_exe, {"check", broken, "-v"}, chains, 1, hopped_to,
                "Undefined function 'missing'")
            run(host_exe, {"build", library, "--output", path.join(root, "library.exe"), "-v"},
                chains, 2, hopped_to, "builds as a library")

            -- '--toolchain' asks for one by name, whatever the build root says, and answers for a
            -- name that is not installed instead of ignoring it
            local by_option = path.join(root, "by option.exe")
            run(host_exe, {"build", hello, "--output", by_option, "--toolchain", "9.9", "-v"},
                chains, 0, "'--toolchain 9.9'", hopped_to, "built")
            run_program(by_option, 0, "Hello, world!")
            run(host_exe, {"build", hello, "--toolchain", "9.9"}, nothing_installed, 2,
                "'--toolchain 9.9'", "holds no toolchain at all", "ask for 'local'",
                slashed(path.join(empty, "9.9", "ens" .. exe_suffix)))

            -- both ways of keeping the work here
            run(host_exe, {"build", pkg, "--output", path.join(root, "kept.exe"), "-v",
                "--toolchain", "local"}, chains, 0, "built", not_hopped)
            run(host_exe, {"build", pkg, "--output", path.join(root, "pinned.exe"), "-v"},
                {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "local"}, 0, "built", not_hopped)

            -- ENS_TOOLCHAIN is the option's environment spelling, and the option wins over it
            run(host_exe, {"build", hello, "--output", path.join(root, "by variable.exe"), "-v"},
                {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "9.9"}, 0, "ENS_TOOLCHAIN=9.9",
                hopped_to, "built")
            run(host_exe, {"build", hello, "--output", path.join(root, "overridden.exe"), "-v",
                "--toolchain", "local"}, {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "9.9"}, 0,
                "built", not_hopped)

            -- a version declared but not installed is what a manifest's declaration has always
            -- been: a statement, not a requirement. It builds here, and says so only when asked.
            local anyway = path.join(root, "anyway.exe")
            run(host_exe, {"build", pkg, "--output", anyway, "-v"}, nothing_installed, 0,
                "written for Ens 9.9", "holds no toolchain at all", "this toolchain is building it",
                "built")
            run_program(anyway, 0, "built by a delegate")
            run(host_exe, {"build", pkg, "--output", path.join(root, "quiet.exe")},
                nothing_installed, 0, "built", "!9.9")

            -- the loop guard, from the delegate's side: the copy sees the very declaration that
            -- reached it and asks for 9.9 outright, and still does the work itself
            run(installed_exe, {"build", pkg, "--output", path.join(root, "guarded.exe"), "-v",
                "--toolchain", "9.9"}, {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "local"}, 0,
                "ENS_TOOLCHAIN=local", "built", not_hopped)

            -- and from outside: one hop is all there is, however the chain starts
            local _, chained = run(installed_exe, {"build", pkg, "--output",
                path.join(root, "once.exe"), "-v"}, chains, 0, "built")
            local hops = 0
            local at = 1
            while true do
                local found = chained:find(hopped_to, at, true)
                if not found then
                    break
                end
                hops = hops + 1
                at = found + 1
            end
            if hops ~= 1 then
                table.insert(failures, string.format("the work was handed on %d time(s), and one "
                    .. "hop is all there is:\n%s", hops, chained))
            end

            -- the other two compiling commands reach the same hook. 'run' hops with its program's
            -- arguments intact, and its program's exit code still comes back through the delegate.
            run(host_exe, {"run", pkg, "-v"}, chains, 0, hopped_to, "built by a delegate")
            run(host_exe, {"run", pkg, "-v", "--toolchain", "local"}, chains, 0,
                "built by a delegate", not_hopped)
            run(host_exe, {"test", pkg, "-v"}, chains, 0, hopped_to, "there are no tests in")
            run(host_exe, {"run", hello, "--toolchain", "9.9"}, nothing_installed, 2,
                "'--toolchain 9.9'", "holds no toolchain at all")
            run(host_exe, {"test", pkg, "--toolchain", "9.9"}, nothing_installed, 2,
                "'--toolchain 9.9'", "holds no toolchain at all")

            -- the commands that never delegate, however the environment is set
            run(host_exe, {"version"}, {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "9.9"}, 0,
                "ens 0.1")
            run(host_exe, {"help", "build"}, chains, 0, "--toolchain <version>")

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d delegation assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- builds the language server, which nothing else compiles. An editor holding the binary open
        -- makes the link fail for a reason that has nothing to do with the change, so that case is
        -- reported as a skip: a run that cannot check says so instead of going red.
        local function run_lsp_build(job)
            local log = path.join(out_dir, job.name .. ".log")
            local rc = execMerged("xmake", {"build", "ens-lsp"}, log)
            if rc == 0 then
                return {name = job.name, ok = true}
            end
            local out = io.readfile(log) or ""
            for _, locked in ipairs({"LNK1104", "Permission denied", "Access is denied",
                                     "being used by another process"}) do
                if out:find(locked, 1, true) then
                    return {name = job.name, ok = true,
                        note = "skipped: the language server binary is in use"}
                end
            end
            return {name = job.name, ok = false,
                short = string.format("build exit %s", tostring(rc)),
                full = string.format("%s: %s", job.name, (out:gsub("[\r\n]+$", "")))}
        end

        -- run a single test: compile, optionally run, and compare against the header.
        -- returns { name = ..., ok = bool, short = <fail reason>, full = <detailed report> }.
        local function run_one(job)
            if job.lsp_build then return run_lsp_build(job) end
            if job.corpus then return run_corpus(job) end
            if job.semacheck then return run_semacheck(job) end
            if job.cli_build then return run_cli_build(job) end
            if job.cli_runtest then return run_cli_runtest(job) end
            if job.cli_toolchain then return run_cli_toolchain(job) end
            if job.cli_overriding then return run_cli_overriding(job) end
            if job.cli_dependencies then return run_cli_dependencies(job) end
            if job.cli_prebuilt then return run_cli_prebuilt(job) end
            if job.codegencheck then return run_codegencheck(job) end
            if job.bootstrap then return run_bootstrap(job) end
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
            local expected_errors   = {}
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
                if error_str then table.insert(expected_errors, error_str) end
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

            -- `ens test` on a folder, asserting its two streams: the unit tests of every selfhost
            -- package and of the standard library, and the @ens-test fixtures. The command under
            -- test runs them, so its sema and code generator see every one of those tests. Its
            -- builds link through ens-lld and some of the suites bind native libraries, so the job
            -- carries the same linker and loader environment the codegen harness does.
            if ens_test_args ~= nil then
                os.tryrm(stdout_file)
                os.tryrm(stderr_file)
                local argv = {"test", job.source}
                for _, a in ipairs(ens_test_args) do
                    table.insert(argv, (a:gsub("{dir}", (job.source:gsub("\\", "/")))))
                end
                local env, _, env_error = llvmEnvironment()
                if not env then
                    return {name = name, ok = false, short = "no LLVM environment",
                        full = string.format("%s: %s", name, env_error)}
                end
                local run_rc = execSplit(host_exe, argv, stdout_file, stderr_file, {envs = env})
                local actual_stdout = captured(stdout_file)
                local actual_stderr = captured(stderr_file)
                local why = compareRun(run_rc, actual_stdout, actual_stderr)
                if #why == 0 then
                    return {name = name, ok = true,
                        note = actual_stdout:match("(%d+/%d+ tests passed)")}
                end
                local short = table.concat(why, "; ")
                return {name = name, ok = false, short = short,
                    full = string.format("%s: %s\nstdout:\n%s\nstderr:\n%s",
                        name, short, actual_stdout, actual_stderr)}
            end

            os.tryrm(exe_file)
            os.tryrm(stdout_file)
            os.tryrm(stderr_file)

            -- the objects go beside the executable rather than under a build root of their own:
            -- every fixture is built for the same target and level, and one object name per
            -- fixture keeps the jobs that share this folder out of each other's way.
            local env, _, env_error = llvmEnvironment()
            if not env then
                return {name = name, ok = false, short = "no LLVM environment",
                    full = string.format("%s: %s", name, env_error)}
            end
            local compile_rc = execMerged(host_exe,
                {"build", job.source, "--output", exe_file, "--objects", out_dir}, compile_log,
                {envs = env})
            local compile_log_text = io.readfile(compile_log) or ""

            if #expected_errors > 0 then
                local why = {}
                if os.isfile(exe_file) then
                    table.insert(why, "compile succeeded but @expect-error was set")
                end
                for _, expected in ipairs(expected_errors) do
                    if not compile_log_text:find(expected, 1, true) then
                        table.insert(why, string.format("error %q not found in stderr", expected))
                    end
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

        -- the compiler has to exist before any job that compiles Ens starts, and the jobs run in
        -- parallel, so the seed is placed and the compiler built here rather than on demand inside
        -- one of them.
        locateSeed()
        print(string.format("Building the compiler with the committed %s seed...", seed_host))
        local host_started = os.mclock()
        buildHostCompiler()
        print(string.format("Compiler built in %.0fs", (os.mclock() - host_started) / 1000.0))

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
