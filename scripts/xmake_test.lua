-- compile each tests/*.ens with `ens` and verify the exit code, the standard output and the
-- standard error against the directives in the test source header:
--     // @exit 12
--     // @stdout Hello!
--     // @stdout-contains Hello
--     // @stderr panic: boom
--     // @stderr-contains panic:
-- @stdout and @stderr give one exact line each, taken as written after the one space that follows the
-- directive so a line that starts with spaces is pinned with them, and accumulate: every directive of the kind,
-- joined with newlines, must equal the whole stream (trailing newlines trimmed). @stdout-contains
-- and @stderr-contains each name a substring the stream must contain. Both streams are captured
-- separately, so what the runtime reports on stderr - a panic, an unhandled exception - is
-- asserted with the @stderr directives and never appears in stdout.
-- each of the four can be scoped to one optimization level by writing it with '-at' and the level,
-- for output that differs between the levels the suite builds fixtures at, such as a stack trace
-- naming a frame that was inlined. A scoped pin applies only at its level and an unscoped one at
-- every level, and the lines of an exact pin keep their order across both kinds:
--     // @stderr-at O0   at helper (main.ens:4)
--     // @stderr-at O2   at helper (main.ens:4, inlined)
--     // @stdout-contains-at O2 inlined
-- this job builds fixtures at O2 and codegencheck_unoptimized at O0, so a pin scoped to any other
-- level is refused as a fixture error, and so are a scoped pin with no line after its level and
-- '-at' written on any other directive.
-- use @expect-error instead to assert the compiler reports a specific diagnostic. These accumulate:
-- every one of them names a substring the compiler's output must contain, so a file whose problems
-- are reported together says so by listing them all.
--     // @expect-error Undefined function 'testFunction'
-- @expect-note asserts a note line the same way, for a diagnostic that points at a second place.
-- Both are substrings of the whole log, so a note is not tied to one error; a fixture carrying a
-- note without an error is refused as a fixture error.
--     // @expect-note an array of 'T' is created here
-- a folder test's main.ens may use @ens-test (optionally with extra arguments) to run
-- `ens test <folder> ...` instead of compile+run, asserting on its two streams the same way.
-- the token {dir} in the extra arguments expands to the folder's absolute path.
-- the token {language_version} in @expect-error and @expect-note expands to the language version
-- the tree's compiler implements, as in 0.13, so a pin naming it holds across a release.
--     // @ens-test --filter needle
-- @stdlib names a folder holding a std/ package the fixture is compiled against instead of libs/,
-- which is how a fixture exercises a standard library the tree's own cannot yet contain. The path
-- is relative to the folder of the file the directive is written in, and semacheck and
-- codegencheck honor it too.
--     // @stdlib ../stdlib
-- @platform limits a fixture to the hosts it names, any of 'linux', 'macos' and 'windows', for a
-- fixture that exercises one system's own behavior. On every other host the fixture is neither
-- built nor run, codegencheck leaves it out the same way, and both list it as skipped for this
-- platform.
--     // @platform linux
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
        import("core.base.json")
        import("core.base.process")
        import("core.tool.toolchain")
        import("net.http")
        import("async.runjobs")
        config.load()
        local mode = config.get("mode") or "release"
        local plat = config.get("plat") or os.host()
        local arch = config.get("arch") or os.arch()
        local build_dir = path.join(os.projectdir(), "build", plat, arch, mode)
        -- every ens the suite starts compiles against this tree's standard library where it is, so
        -- a note into the library names a file in the tree; the cli_build assertions about the copy
        -- ens carries unset it. What a link writes into the cache, such as the libSystem a macOS
        -- program links against, goes into a cache of the run's own, never the user's.
        os.setenv("ENS_STDLIB", path.join(os.projectdir(), "libs"))
        os.setenv("ENS_CACHE", path.join(os.projectdir(), "build", "cache"))
        -- the newest toolchain installed on this machine would run every command the suite starts,
        -- so every `ens` here looks in a toolchains folder of the run's own that holds none, and
        -- the cli_toolchain job installs the ones it needs in folders of its own.
        local no_toolchains = path.join(os.projectdir(), "build", "notoolchains")
        os.tryrm(no_toolchains)
        os.mkdir(no_toolchains)
        os.setenv("ENS_TOOLCHAINS", no_toolchains)
        os.setenv("ENS_TOOLCHAIN", "")
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
        -- the native archive every Ens program that links LLVM or the linker bridge links: the
        -- bridge, LLVM and lld in one library. It is a separate target from the compiler, so a
        -- fresh clone needs it built before anything can reach an executable.
        local native_archive = path.join(build_dir, is_host("windows") and "ens-llvm.lib"
            or "libens-llvm.a")
        -- a program of the LLVM package, such as llvm-nm, or nil when the package holds none
        local function llvmTool(name)
            return os.files(path.join(global.directory(), "packages", "l", "llvm", "*", "*", "bin",
                name .. exe_suffix))[1]
        end

        -- the seed: the published `ens` this tree pins for this host. Its one job is to build the
        -- compiler out of this tree, and nothing else in the suite runs it. Nothing it produces is
        -- ever compared against anything either, so a seed left behind by the tree shows up as a
        -- failure to build rather than as drift in what the tests measure.
        local seed_platforms = {windows = "windows", linux = "linux", macosx = "macos"}
        local seed_architectures = {x64 = "x64", x86_64 = "x64", arm64 = "arm64"}
        local seed_host = (seed_platforms[plat] or plat) .. "-" .. (seed_architectures[arch] or arch)
        local seed_dir = path.join(os.projectdir(), "build", "seed")
        local seed_exe = path.join(seed_dir, "ens" .. exe_suffix)

        -- what `ens version` prints: the release this tree is, read from the one file that declares
        -- it, and the system and processor it runs on.
        local version_source = io.readfile(path.join(os.projectdir(), "selfhost", "language",
            "src", "version.ens")) or ""
        local language_version = version_source:match('string release = "(%d+%.%d+)') or "?"
        local version_line = string.format("ens %s %s/%s",
            version_source:match('string release = "([^"]*)"') or "?",
            seed_platforms[plat] or plat, seed_architectures[arch] or arch)

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
        -- effects the tests/ fixtures drive against a real operating system. An environment's name
        -- matching is checked for every platform here, since each Environment is created with the
        -- platform whose matching rule it follows.
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

        -- the language package's tests: the one version Ens has, and the language version read
        -- out of it.
        if want("selfhost_language") then
            table.insert(jobs, {
                name = "selfhost_language",
                source = path.join(os.projectdir(), "selfhost", "language"),
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

        -- the carried files' tests: writing them into a real store under their digest, reading
        -- the same folder on a later run, and stepping over a folder an interrupted run left.
        if want("selfhost_bundled") then
            table.insert(jobs, {
                name = "selfhost_bundled",
                source = path.join(os.projectdir(), "selfhost", "bundled"),
                ens_test_args = {},
            })
        end

        -- the target library's unit tests: the processor and the system a triple names, over every
        -- spelling of each.
        if want("selfhost_target") then
            table.insert(jobs, {
                name = "selfhost_target",
                source = path.join(os.projectdir(), "selfhost", "target"),
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
        -- assert the LLVM verifier accepts them. They link the native ens-llvm library, so the job
        -- points the linker at it the same way codegencheck does.
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
        -- binds the native ens-llvm library, so the job carries the same native plumbing.
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

        -- `ens` as one file: the compiler loads only libraries every machine of its platform has,
        -- and it runs from a folder of its own with none of this build's library paths set.
        if want("standalone") then
            table.insert(jobs, {
                name = "standalone",
                standalone = true,
            })
        end

        -- a Linux program linked on whatever machine this is, for both Linux targets, through only
        -- the files ens carries: what it loads and the glibc it needs, read back from the file, and
        -- on a Linux machine of the same processor, what it prints when it runs.
        if want("linux_link") then
            table.insert(jobs, {
                name = "linux_link",
                linux_link = true,
            })
        end

        -- a Windows program linked on whatever machine this is, for both Windows targets, through
        -- only the files ens carries: which DLLs it imports from, read back from the file, and on
        -- a Windows machine of the same processor, what it prints when it runs.
        if want("windows_link") then
            table.insert(jobs, {
                name = "windows_link",
                windows_link = true,
            })
        end

        -- every symbol the standard library and a program's entry code call on each of the five
        -- targets is one the system libraries ens carries for that target provide, or one ens's
        -- own startup object defines. A library call code generation adds fails here on every
        -- host, rather than at a user's link.
        if want("symbol_audit") then
            table.insert(jobs, {
                name = "symbol_audit",
                symbol_audit = true,
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
        -- a tag that declares something else.
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
        -- scratch folder: the hop, the command line it forwards, the exit code it propagates, and
        -- the guard that stops a delegate delegating again.
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

        -- the name a @platform directive gives the host running the suite.
        local function hostPlatform()
            if is_host("windows") then return "windows" end
            if is_host("macosx") then return "macos" end
            return "linux"
        end

        -- the hosts the @platform lines of a fixture's directive file name, and apart from them
        -- every name that is no host's.
        local function fixturePlatforms(file)
            local named, unknown = {}, {}
            for line in (io.readfile(file) or ""):gmatch("[^\r\n]+") do
                local listed = line:match("^%s*//%s*@platform%s+(.*)$")
                if listed then
                    for platform in listed:gmatch("%S+") do
                        if platform == "linux" or platform == "macos" or platform == "windows" then
                            table.insert(named, platform)
                        else
                            table.insert(unknown, platform)
                        end
                    end
                end
            end
            return named, unknown
        end

        -- whether a fixture naming these hosts runs on this one; naming none runs everywhere.
        local function runsHere(named)
            if #named == 0 then return true end
            for _, platform in ipairs(named) do
                if platform == hostPlatform() then return true end
            end
            return false
        end

        -- the oldest macOS every member of ens-llvm is built for, read from the archive itself, and
        -- the members that name each version. xmake.lua builds every member for one version, so
        -- more than one is a build defect, reported by the caller.
        local function archiveVersions()
            local objdump = llvmTool("llvm-objdump")
            if not objdump then
                os.raise("no llvm-objdump in the LLVM package to read ens-llvm's macOS version with")
            end
            local archive = path.join(build_dir, "libens-llvm.a")
            local listing = os.iorunv(objdump, {"--macho", "--private-headers", archive})
            local versions = {}
            local member = "?"
            for line in listing:gmatch("[^\r\n]+") do
                local named = line:match("^.-%((.-)%):$")
                if named then
                    member = named
                else
                    local version = line:match("^%s*minos%s+([%d.]+)")
                    if version then
                        versions[version] = versions[version] or {}
                        table.insert(versions[version], member)
                    end
                end
            end
            return versions
        end

        -- the target the tree's own programs are built for. On macOS that is the triple naming the
        -- version every member of ens-llvm is built for, so `ens` and the package tests that link
        -- it claim no older macOS than their libraries run on; elsewhere it is this machine's own.
        local tree_target = nil
        local function treeTargetArguments()
            if not is_host("macosx") then return {} end
            if tree_target then return tree_target end
            local versions = archiveVersions()
            local found = {}
            local floor = nil
            for version, members in pairs(versions) do
                table.insert(found, string.format("%s (%d members, such as %s)", version,
                    #members, members[1]))
                floor = version
            end
            if #found ~= 1 then
                table.sort(found)
                os.raise("ens-llvm's members are built for %s; xmake.lua builds every one of them "
                    .. "for one macOS version", #found == 0 and "no macOS version"
                    or table.concat(found, ", "))
            end
            tree_target = {"--target", os.arch() .. "-apple-macosx" .. floor}
            return tree_target
        end

        -- the environment a process that links an Ens program needs, which is the folder holding
        -- the ens-llvm archive on the linker's search path: LIBRARY_PATH on Linux and macOS, which
        -- ens turns into -L, and LIB on Windows, where lld-link finds nothing else once LIB is set,
        -- so the MSVC and Windows SDK folders are added to it too.
        local function linkEnvironment()
            local env = os.getenvs()
            if is_host("windows") then
                local msvc = toolchain.load("msvc", {plat = plat, arch = arch})
                local msvc_lib = ""
                if msvc then
                    local envs = msvc:runenvs()
                    if envs and envs.LIB then msvc_lib = envs.LIB end
                end
                env.LIB = msvc_lib .. ";" .. build_dir
                return env
            end
            if env.LIBRARY_PATH and #env.LIBRARY_PATH > 0 then
                env.LIBRARY_PATH = build_dir .. ":" .. env.LIBRARY_PATH
            else
                env.LIBRARY_PATH = build_dir
            end
            return env
        end

        -- whether ENS_SEEDS holds a seed for `tag`, which is how a host the pin names no seed for
        -- bootstraps, from one built for it elsewhere. Nothing pins what that seed is, so it is the
        -- one case where the seed is not checked against a hash.
        local function unpinnedSeed(tag)
            local named = os.getenv("ENS_SEEDS")
            return named ~= nil and #named > 0
                and os.isfile(path.join(path.normalize(named), tag, "ens" .. exe_suffix))
        end

        -- what the tree pins the seed to: the release tag it was published under and the sha256 of
        -- each host's binary. One file covers every host, because a refresh replaces every one
        -- of them together and a per-host pin would let one host bootstrap from a seed the others
        -- have already moved past.
        local function seedPin()
            local pin_file = path.join(os.projectdir(), "seed", "seed.json")
            local text = io.readfile(pin_file)
            if not text then
                os.raise("the bootstrap seed pin is missing: '%s' does not exist. It names the "
                    .. "release every host's seed is downloaded from, and nothing can be built "
                    .. "without it.", pin_file)
            end
            local pin = try {
                function() return json.decode(text) end,
                catch {
                    function(errors)
                        os.raise("the bootstrap seed pin at '%s' is not valid JSON: %s", pin_file,
                            ((tostring(errors):gsub("%s+$", ""))))
                    end
                }
            }
            if type(pin) ~= "table" or type(pin.tag) ~= "string" or type(pin.url) ~= "string"
                    or type(pin.hosts) ~= "table" then
                os.raise("the bootstrap seed pin at '%s' is missing what it has to say: a 'tag' "
                    .. "string, a 'url' string, and a 'hosts' object mapping each host to the "
                    .. "sha256 of its seed.", pin_file)
            end
            local sha256 = pin.hosts[seed_host]
            if type(sha256) ~= "string" and unpinnedSeed(pin.tag) then
                return pin.tag, pin.url, nil
            end
            if type(sha256) ~= "string" then
                local named = {}
                for host in pairs(pin.hosts) do
                    table.insert(named, "'" .. host .. "'")
                end
                table.sort(named)
                os.raise("no bootstrap seed for %s: '%s' pins %s. Seeds are not cross-compiled, so "
                    .. "one has to be built on this host and published before it can bootstrap; "
                    .. "until then, point ENS_SEEDS at a folder holding '%s/ens%s'.", seed_host,
                    pin_file, table.concat(named, ", "), pin.tag, exe_suffix)
            end
            return pin.tag, pin.url, sha256
        end

        -- where downloaded seeds are kept: the folder ENS_SEEDS names, else '.ens/seeds' under this
        -- user's own folder, one folder per release tag. They sit beside the toolchains a version
        -- hop reaches for rather than among them: a toolchain there is a language version, a seed is
        -- one particular build, and several seeds are the same language version.
        local function seedRoot()
            local named = os.getenv("ENS_SEEDS")
            if named and #named > 0 then
                return path.normalize(named)
            end
            local variable = is_host("windows") and "USERPROFILE" or "HOME"
            local user = os.getenv(variable)
            if not user or #user == 0 then
                os.raise("nowhere to keep the bootstrap seed: set ENS_SEEDS to the folder seeds "
                    .. "live in, or set %s so they are kept under '.ens/seeds' there.", variable)
            end
            return path.join(path.join(path.normalize(user), ".ens"), "seeds")
        end

        -- download this host's seed for `tag` and install it at `installed`. It lands beside where
        -- it is going and is renamed into place only once it hashes to what the tree pins, so an
        -- interrupted or tampered download cannot leave behind something a later run mistakes for a
        -- verified seed. Beside rather than in a temporary folder because the rename has to stay on
        -- one filesystem: a system temporary folder is frequently a different one, and renaming
        -- across two fails.
        local function fetchSeed(tag, url, sha256, installed)
            local asset = string.format("%s/%s/ens-%s%s", url, tag, seed_host, exe_suffix)
            local partial = installed .. ".partial"
            os.mkdir(path.directory(installed))
            os.tryrm(partial)
            print(string.format("Downloading the %s bootstrap seed %s...", seed_host, tag))
            try {
                function() http.download(asset, partial) end,
                catch {
                    function(errors)
                        os.tryrm(partial)
                        os.raise("could not download the %s bootstrap seed from '%s': %s. The "
                            .. "suite needs it to build the compiler; with no network, point "
                            .. "ENS_SEEDS at a folder holding '%s/ens%s'.", seed_host, asset,
                            ((tostring(errors):gsub("%s+$", ""))), tag, exe_suffix)
                    end
                }
            }
            local found = hash.sha256(partial)
            if found ~= sha256 then
                os.tryrm(partial)
                os.raise("the %s bootstrap seed downloaded from '%s' is not what this tree pins.\n"
                    .. "  pinned sha256 %s\n  downloaded    %s\nThe release asset was replaced or "
                    .. "the download is corrupt; nothing is run until the two agree.", seed_host,
                    asset, sha256, found)
            end
            os.mv(partial, installed)
        end

        -- place the seed: the `ens` this tree pins for this host, put where the build below looks
        -- for it. A host with no seed cannot run the suite, and saying so is the right outcome:
        -- falling back to another compiler would leave the seed untested.
        local function locateSeed()
            local tag, url, sha256 = seedPin()
            local installed = path.join(path.join(seedRoot(), tag), "ens" .. exe_suffix)
            if not sha256 then
                print(string.format("Using the %s bootstrap seed at '%s', which '%s' pins no "
                    .. "hash for.", seed_host, installed, path.join("seed", "seed.json")))
            else
                if not os.isfile(installed) then
                    fetchSeed(tag, url, sha256, installed)
                end
                -- checked on every run and not only when downloaded: this is the one binary the
                -- suite runs that the tree did not build, so what it is stays verified rather than
                -- assumed.
                local found = hash.sha256(installed)
                if found ~= sha256 then
                    os.raise("the %s bootstrap seed at '%s' is not what this tree pins.\n  pinned "
                        .. "sha256 %s\n  found         %s\nDelete it to have the pinned one "
                        .. "downloaded again.", seed_host, installed, sha256, found)
                end
            end
            os.tryrm(seed_dir)
            os.mkdir(seed_dir)
            os.cp(installed, seed_exe)
            if not is_host("windows") then
                -- a release asset carries no permissions, so the executable bit is set here.
                os.runv("chmod", {"+x", seed_exe})
            end
        end

        -- the native archive, built every run. The build is incremental. A run whose bridge
        -- sources are unchanged pays nothing for it. A run that skipped it would link every fixture
        -- through whatever archive an earlier build left behind.
        local function buildNativeArchive()
            os.execv("xmake", {"build", "ens-llvm"})
            if not os.isfile(native_archive) then
                os.raise("Could not locate the native archive at " .. native_archive)
            end
        end

        -- build the compiler every job then drives: the seed compiles `selfhost/driver` out of this
        -- tree, so what the suite measures is the sources rather than a binary in the repository.
        -- This is also the whole of what the pinned seed has to do, and it is checked here by
        -- construction: a seed that cannot build the tree stops the run before any job starts.
        local function buildHostCompiler()
            local log = path.join(out_dir, "host.log")
            local env = linkEnvironment()
            os.tryrm(host_dir)
            os.mkdir(host_dir)
            -- for this machine's default target: the seed links it with the line of the release
            -- it came from, and only this tree's own compiler links for the floor below
            local rc = execMerged(seed_exe, {"build",
                path.join(os.projectdir(), "selfhost", "driver"), "--output", host_exe,
                "--stdlib", path.join(os.projectdir(), "libs"), "--objects", host_dir}, log,
                {envs = env})
            if rc ~= 0 or not os.isfile(host_exe) then
                os.raise("the %s seed could not build the compiler from this tree (exit %s). A seed "
                    .. "older than the sources has to be replaced:\n%s", seed_host, tostring(rc),
                    captured(log))
            end
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
            local env = linkEnvironment()
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
            local env = linkEnvironment()
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
            for _, pkg in ipairs({"corpus", "frontend", "language", "sema", "semacheck", "syntaxgen",
                              "target", "llvm", "codegen", "codegencheck", "host", "cli",
                              "packages", "bundled", "link", "build", "driver"}) do
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

            local env = linkEnvironment()

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

            -- list every candidate fixture, naming the file whose header carries the directives;
            -- the harness reads each one and works out for itself what the fixture asks for.
            local lines = {
                "spike " .. spike_exe,
                "stdlib " .. (path.join(os.projectdir(), "libs"):gsub("\\", "/")),
                "scratch " .. scratch,
                "skiplist " .. skiplist,
                "optimization " .. job.optimization,
            }
            -- a fixture for another host is named apart, so it is neither run nor counted as a
            -- fixture that should have been.
            local function listFixture(label, directive)
                if runsHere((fixturePlatforms(directive))) then
                    table.insert(lines, "fixture " .. label .. " " .. directive)
                else
                    table.insert(lines, "elsewhere " .. label)
                end
            end
            local singles = os.files(path.join(tests_dir, "*.ens"))
            table.sort(singles)
            for _, f in ipairs(singles) do
                listFixture("tests/" .. path.filename(f), f)
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
                    listFixture("tests/" .. path.basename(sub), directive)
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

        -- where two files differ: how many bytes, the first and last offsets, the section the
        -- first one is in, and the header lines llvm-readobj reads differently, so a link that
        -- depends on something other than its inputs says what it read.
        local function describeDifference(file2, bytes2, file3, bytes3)
            local count, first, last = 0, nil, nil
            local chunk = 4096
            local shorter = math.min(#bytes2, #bytes3)
            for at = 1, shorter, chunk do
                local stop = math.min(at + chunk - 1, shorter)
                if bytes2:sub(at, stop) ~= bytes3:sub(at, stop) then
                    for i = at, stop do
                        if bytes2:byte(i) ~= bytes3:byte(i) then
                            count = count + 1
                            first = first or i
                            last = i
                        end
                    end
                end
            end
            if #bytes2 ~= #bytes3 then
                count = count + math.abs(#bytes2 - #bytes3)
                first = first or shorter + 1
                last = math.max(#bytes2, #bytes3)
            end
            local readobj = llvmTool("llvm-readobj")
            if not readobj then
                return string.format("%d bytes differ, the first at offset %d and the last at %d.",
                    count, first - 1, last - 1)
            end
            local section = "no section"
            local current = nil
            for line in os.iorunv(readobj, {"--sections", file2}):gmatch("[^\r\n]+") do
                local name = line:match("^%s*Name:%s*(%S+)")
                if name then
                    current = {name = name}
                elseif current then
                    local offset = line:match("^%s*Offset:%s*(%S+)")
                        or line:match("^%s*PointerToRawData:%s*(%S+)")
                    local size = line:match("^%s*Size:%s*(%S+)")
                        or line:match("^%s*RawDataSize:%s*(%S+)")
                    current.offset = current.offset or (offset and tonumber(offset))
                    current.size = current.size or (size and tonumber(size))
                    if current.offset and current.size and first - 1 >= current.offset
                            and first - 1 < current.offset + current.size then
                        section = string.format("section %s, %d bytes in", current.name,
                            first - 1 - current.offset)
                    end
                end
            end
            local arguments = {"--file-headers"}
            if is_host("windows") then table.insert(arguments, "--coff-debug-directory") end
            local function headerLines(file)
                local lines = {}
                for line in os.iorunv(readobj, table.join(arguments, {file})):gmatch("[^\r\n]+") do
                    table.insert(lines, line)
                end
                return lines
            end
            local lines2, lines3 = headerLines(file2), headerLines(file3)
            local changed = {}
            for i = 1, math.max(#lines2, #lines3) do
                if lines2[i] ~= lines3[i] and #changed < 12
                        and not (lines2[i] or ""):match("^File:") then
                    table.insert(changed, string.format("  stage 2: %s\n  stage 3: %s",
                        (lines2[i] or ""):match("^%s*(.-)%s*$"), (lines3[i] or ""):match("^%s*(.-)%s*$")))
                end
            end
            return string.format("%d bytes differ, the first at offset %d (%s) and the last at "
                .. "%d.%s", count, first - 1, section, last - 1, #changed == 0 and ""
                or "\nHeader lines read differently:\n" .. table.concat(changed, "\n"))
        end

        -- the bootstrap fixpoint: the Ens-written compiler compiles itself, twice, with identical
        -- output. Stage 2 has the compiler under test compile its own sources (stage2/ens plus the
        -- object files it wrote), stage 3 has stage2's ens compile the same sources again. The gate
        -- holds when the two stages' objects hold the same modules with identical bytes and the
        -- two executables are identical as well: the link writes no time or place into them, so a
        -- difference there is a link that depends on something other than its inputs. Both stages
        -- are linked with --strip, as stage 2 is what a release publishes, so the executables
        -- compared are the ones a release ships.
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

            local env = linkEnvironment()

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

            local target = treeTargetArguments()
            local failed = staged("stage 2 (the compiler compiles itself)", host_exe,
                    table.join({"build", driver_src, "--output", boot2, "--stdlib", libs,
                     "--objects", stage2_dir, "--strip"}, target), boot2)
                or staged("stage 3 (stage 2 compiles the same sources)", boot2,
                    table.join({"build", driver_src, "--output", boot3, "--stdlib", libs,
                     "--objects", stage3_dir, "--strip"}, target), boot3)
            if failed then
                return {name = name, ok = false, short = failed:match("^[^\n]+"),
                    full = string.format("%s: %s", name, failed)}
            end

            -- the fixpoint: the two stages' objects, module by module in name order. A build keeps
            -- an object under its emitted name with the digest of its bytes before the extension, so
            -- the two stages are matched by the emitted name and their bytes then compared.
            local function emittedName(kept)
                local front, digest, extension = kept:match("^(.*)%.(%x+)(%.[^.]+)$")
                if front and #digest == 16 then return front .. extension end
                return kept
            end
            local kept_names = {}
            local function objectNames(folder)
                local names = {}
                for _, f in ipairs(os.files(path.join(folder, "*" .. obj_suffix))) do
                    local emitted = emittedName(path.filename(f))
                    kept_names[folder .. "|" .. emitted] = path.filename(f)
                    table.insert(names, emitted)
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
                local bytes2 = readBytes(path.join(stage2_dir, kept_names[stage2_dir .. "|" .. n]))
                local bytes3 = readBytes(path.join(stage3_dir, kept_names[stage3_dir .. "|" .. n]))
                total_bytes = total_bytes + #bytes2
                if bytes2 ~= bytes3 then
                    return {name = name, ok = false,
                        short = string.format("fixpoint failed at %s", n),
                        full = string.format("%s: %s differs between stage 2 (%d bytes) and "
                            .. "stage 3 (%d bytes); the pipeline is not deterministic or the "
                            .. "two compilers disagree", name, n, #bytes2, #bytes3)}
                end
            end
            local bytes2 = readBytes(boot2)
            local bytes3 = readBytes(boot3)
            if bytes2 ~= bytes3 then
                -- the same compiler links the same objects once more: a second result equal to
                -- the first points at the two compilers, one unequal to it at the link itself
                local again_dir = path.join(boot_dir, "stage3-again")
                os.mkdir(again_dir)
                local again = path.join(again_dir, "ens" .. exe_suffix)
                local relinked = staged("stage 3 again (stage 2 links the same objects once more)",
                    boot2, table.join({"build", driver_src, "--output", again, "--stdlib", libs,
                     "--objects", stage3_dir, "--strip"}, target), again)
                local repeated = relinked and ("the second link by stage 2 " .. relinked)
                    or readBytes(again) == bytes3 and "A second link of the same objects by stage "
                        .. "2 is identical to the first, so the two compilers link differently."
                    or "A second link of the same objects by stage 2 differs from the first as "
                        .. "well, so one compiler links the same inputs differently each time."
                return {name = name, ok = false,
                    short = "the stage 2 and stage 3 executables differ",
                    full = string.format("%s: every module is identical, but the stage 2 "
                        .. "executable (%d bytes) and the stage 3 executable (%d bytes) differ; "
                        .. "the link depends on something other than its inputs. %s %s", name,
                        #bytes2, #bytes3, describeDifference(boot2, bytes2, boot3, bytes3),
                        repeated)}
            end
            -- on macOS, stage 2 is the compiler a release ships, and it claims to run on the
            -- version every member of ens-llvm is built for, no older and no newer
            if is_host("macosx") then
                local floor = target[2]:match("macosx(.*)$")
                local objdump = llvmTool("llvm-objdump")
                local headers = os.iorunv(objdump, {"--macho", "--private-headers", boot2})
                local claimed = headers:match("\n%s*minos%s+([%d.]+)")
                if claimed ~= floor then
                    return {name = name, ok = false,
                        short = string.format("stage 2 claims macOS %s, its libraries are built "
                            .. "for %s", tostring(claimed), floor),
                        full = string.format("%s: '%s' is linked to run on macOS %s, but every "
                            .. "member of ens-llvm is built for macOS %s", name, boot2,
                            tostring(claimed), floor)}
                end
            end
            -- stage 2 ships without a symbol table, so llvm-nm has nothing it defines to list
            local nm = llvmTool("llvm-nm")
            if not nm then
                return {name = name, ok = false, short = "no llvm-nm in the LLVM package"}
            end
            execMerged(nm, {"--defined-only", "--just-symbol-name", "--quiet", boot2}, log)
            local kept = {}
            for line in captured(log):gmatch("[^\r\n]+") do table.insert(kept, line) end
            if #kept > 0 then
                return {name = name, ok = false,
                    short = string.format("stage 2 carries %d symbols", #kept),
                    full = string.format("%s: '%s' is linked with --strip, but it defines %d "
                        .. "symbols llvm-nm lists, the first '%s'", name, boot2, #kept, kept[1])}
            end
            local note = string.format("stages %.0fs/%.0fs; fixpoint over %d modules, "
                .. "%.1f MB; executables identical", seconds[1] or 0, seconds[2] or 0,
                #names2, total_bytes / (1024 * 1024))
            return {name = name, ok = true, note = note}
        end

        -- the libraries a program loads when it starts, as the LLVM package's llvm-readobj lists
        -- them, or nil and why not.
        local function loadedLibraries(program, log)
            local readobj = llvmTool("llvm-readobj")
            if not readobj then
                return nil, "no llvm-readobj in the LLVM package"
            end
            local option = is_host("windows") and "--coff-imports" or "--needed-libs"
            local rc = execMerged(readobj, {option, program}, log)
            if rc ~= 0 then
                return nil, "llvm-readobj could not read " .. program
            end
            local libraries = {}
            local listing = false
            for line in (io.readfile(log) or ""):gmatch("[^\r\n]+") do
                local trimmed = line:match("^%s*(.-)%s*$")
                if is_host("windows") then
                    local imported = trimmed:match("^Name:%s*(.+)$")
                    if imported then
                        table.insert(libraries, imported)
                    end
                elseif trimmed == "NeededLibraries [" then
                    listing = true
                elseif trimmed == "]" then
                    listing = false
                elseif listing and #trimmed > 0 then
                    table.insert(libraries, trimmed)
                end
            end
            return libraries
        end

        -- whether a library is one every machine of this platform has. Windows names a system DLL
        -- in any letter case.
        local function isSystemLibrary(library)
            if is_host("macosx") then
                return library == "/usr/lib/libSystem.B.dylib"
            end
            if is_host("windows") then
                local name = library:lower()
                if name:startswith("api-ms-win-") then
                    return true
                end
                for _, system in ipairs({"kernel32.dll", "ntdll.dll", "advapi32.dll",
                        "shell32.dll", "ole32.dll", "oleaut32.dll", "user32.dll", "psapi.dll",
                        "version.dll", "ws2_32.dll", "bcrypt.dll", "crypt32.dll", "dbghelp.dll",
                        "shlwapi.dll"}) do
                    if name == system then
                        return true
                    end
                end
                return false
            end
            for _, system in ipairs({"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
                    "librt.so.1", "libgcc_s.so.1"}) do
                if library == system then
                    return true
                end
            end
            return library:match("^ld%-linux.*%.so%.%d+$") ~= nil
        end

        local function run_linux_link(job)
            local name = job.name
            local log = path.join(out_dir, name .. ".log")
            local root = path.join(os.projectdir(), "build", "linuxlink")
            os.tryrm(root)
            os.mkdir(root)
            local readobj = llvmTool("llvm-readobj")
            if not readobj then
                return {name = name, ok = false, short = "no llvm-readobj in the LLVM package"}
            end
            -- the linker names itself in '.comment' by version alone, whatever build of LLVM
            -- this ens carries, so the output is the same bytes from every host
            local llvm_version = readobj:match("[/\\]llvm[/\\](%d+%.%d+%.%d+)[/\\]")
            if not llvm_version then
                return {name = name, ok = false, short = "the LLVM package folder '" .. readobj
                    .. "' names no version"}
            end
            local banner = "Linker: LLD " .. llvm_version
            local failures = {}
            local targets = {
                {triple = "x86_64-unknown-linux-gnu", processor = "x86_64",
                    loader = "/lib64/ld-linux-x86-64.so.2"},
                {triple = "aarch64-unknown-linux-gnu", processor = "arm64",
                    loader = "/lib/ld-linux-aarch64.so.1"},
            }
            local programs = {
                {fixture = "hello", rc = 0, stdout = "Hello, world!", stderr = ""},
                {fixture = "exc_main_unhandled", rc = 1, stdout = "",
                    stderr = "at main (exc_main_unhandled.ens:10)"},
            }
            local allowed = {["libc.so.6"] = true, ["libm.so.6"] = true,
                ["libgcc_s.so.1"] = true}
            for _, target in ipairs(targets) do
                for _, program in ipairs(programs) do
                    local exe = path.join(root, program.fixture .. "-" .. target.processor)
                    local label = program.fixture .. " for " .. target.triple
                    local rc = execMerged(host_exe, {"build",
                        path.join(tests_dir, program.fixture .. ".ens"), "--target", target.triple,
                        "--output", exe}, log)
                    if rc ~= 0 then
                        table.insert(failures, string.format("%s did not build:\n%s", label,
                            captured(log)))
                    else
                        execMerged(readobj, {"--needed-libs", "--version-info",
                            "--string-dump=.interp", exe}, log)
                        local dump = captured(log)
                        local listing = false
                        for line in dump:gmatch("[^\r\n]+") do
                            local trimmed = line:match("^%s*(.-)%s*$")
                            if trimmed == "NeededLibraries [" then
                                listing = true
                            elseif trimmed == "]" then
                                listing = false
                            elseif listing and #trimmed > 0 and not allowed[trimmed]
                                    and trimmed ~= path.filename(target.loader) then
                                table.insert(failures, string.format("%s loads '%s'", label,
                                    trimmed))
                            end
                            local major, minor = trimmed:match("^Name: GLIBC_(%d+)%.(%d+)")
                            if major and (tonumber(major) > 2 or tonumber(minor) > 34) then
                                table.insert(failures, string.format("%s needs %s, newer than "
                                    .. "glibc 2.34", label, trimmed:sub(7)))
                            end
                        end
                        if not dump:find("] " .. target.loader, 1, true) then
                            table.insert(failures, string.format("%s does not start through "
                                .. "'%s'", label, target.loader))
                        end
                        execMerged(readobj, {"--string-dump=.comment", exe}, log)
                        local comments = {}
                        for line in captured(log):gmatch("[^\r\n]+") do
                            local text = line:match("^%[%s*%d+%]%s(.*)$")
                            if text then table.insert(comments, text) end
                        end
                        if #comments ~= 1 or comments[1] ~= banner then
                            table.insert(failures, string.format("%s names its linker as %q, "
                                .. "expected %q", label, table.concat(comments, " / "), banner))
                        end
                        if is_host("linux") and os.arch() == target.processor then
                            local stdout_file = path.join(root, program.fixture .. ".out")
                            local stderr_file = path.join(root, program.fixture .. ".err")
                            local ran = execSplit(exe, {}, stdout_file, stderr_file)
                            local printed = captured(stdout_file)
                            local reported = captured(stderr_file)
                            if ran ~= program.rc or printed ~= program.stdout
                                    or not reported:find(program.stderr, 1, true) then
                                table.insert(failures, string.format("%s exited %s, printing "
                                    .. "%q and reporting %q", label, tostring(ran), printed,
                                    reported))
                            end
                        end
                    end
                end
            end
            if #failures > 0 then
                return {name = name, ok = false, short = failures[1]:match("^[^\n]*"),
                    full = name .. ": " .. table.concat(failures, "\n")}
            end
            return {name = name, ok = true}
        end

        local function run_windows_link(job)
            local name = job.name
            local log = path.join(out_dir, name .. ".log")
            local root = path.join(os.projectdir(), "build", "windowslink")
            os.tryrm(root)
            os.mkdir(root)
            local readobj = llvmTool("llvm-readobj")
            if not readobj then
                return {name = name, ok = false, short = "no llvm-readobj in the LLVM package"}
            end
            local failures = {}
            local targets = {
                {triple = "x86_64-pc-windows-msvc", processor = "x64"},
                {triple = "aarch64-pc-windows-msvc", processor = "arm64"},
            }
            local programs = {
                {fixture = "hello", rc = 0, stdout = "Hello, world!", stderr = ""},
                {fixture = "exc_main_unhandled", rc = 1, stdout = "",
                    stderr = "at main (exc_main_unhandled.ens:10)"},
            }
            -- the DLLs every Windows has: the system's own, and the C runtime it ships with
            local function systemDll(dll)
                local lower = dll:lower()
                return lower == "kernel32.dll" or lower == "shell32.dll"
                    or lower:startswith("api-ms-win-crt-")
            end
            for _, target in ipairs(targets) do
                for _, program in ipairs(programs) do
                    local exe = path.join(root, program.fixture .. "-" .. target.processor .. ".exe")
                    local label = program.fixture .. " for " .. target.triple
                    local rc = execMerged(host_exe, {"build",
                        path.join(tests_dir, program.fixture .. ".ens"), "--target", target.triple,
                        "--output", exe}, log)
                    if rc ~= 0 then
                        table.insert(failures, string.format("%s did not build:\n%s", label,
                            captured(log)))
                    else
                        execMerged(readobj, {"--coff-imports", exe}, log)
                        for line in captured(log):gmatch("[^\r\n]+") do
                            local dll = line:match("^%s*Name:%s*(.+)$")
                            if dll and not systemDll(dll) then
                                table.insert(failures, string.format("%s imports from '%s'",
                                    label, dll))
                            end
                        end
                        -- the startup code runs the initializers between its two null pointers
                        -- in '.CRT', and every module that traces registers its tables through
                        -- one of them, so the section holds a null, at least one pointer, a null
                        local pointers = {}
                        execMerged(readobj, {"--hex-dump=.CRT", exe}, log)
                        for line in captured(log):gmatch("[^\r\n]+") do
                            local words = line:match("^0x%x+%s+([%x ]+%x)")
                            if words then
                                local low = nil
                                for word in words:gmatch("%x%x%x%x%x%x%x%x") do
                                    if low then
                                        table.insert(pointers, low ~= "00000000" or word ~= "00000000")
                                        low = nil
                                    else
                                        low = word
                                    end
                                end
                            end
                        end
                        local present = 0
                        for i = 2, #pointers - 1 do
                            if pointers[i] then present = present + 1 end
                        end
                        if #pointers < 3 or pointers[1] or pointers[#pointers] or present == 0 then
                            table.insert(failures, string.format("%s has %d pointers in '.CRT', "
                                .. "expected a null, the initializers, and a null", label,
                                #pointers))
                        end
                        if is_host("windows") and os.arch() == target.processor then
                            local stdout_file = path.join(root, program.fixture .. ".out")
                            local stderr_file = path.join(root, program.fixture .. ".err")
                            local ran = execSplit(exe, {}, stdout_file, stderr_file)
                            local printed = captured(stdout_file):gsub("\r\n", "\n")
                            local reported = captured(stderr_file)
                            if ran ~= program.rc or printed ~= program.stdout
                                    or not reported:find(program.stderr, 1, true) then
                                table.insert(failures, string.format("%s exited %s, printing "
                                    .. "%q and reporting %q", label, tostring(ran), printed,
                                    reported))
                            end
                        end
                    end
                end
            end
            if #failures > 0 then
                return {name = name, ok = false, short = failures[1]:match("^[^\n]*"),
                    full = name .. ": " .. table.concat(failures, "\n")}
            end
            return {name = name, ok = true}
        end

        local function run_symbol_audit(job)
            local name = job.name
            local log = path.join(out_dir, name .. ".log")
            local root = path.join(os.projectdir(), "build", "symbolaudit")
            os.tryrm(root)
            os.mkdir(root)
            local nm = llvmTool("llvm-nm")
            if not nm then
                return {name = name, ok = false, short = "no llvm-nm in the LLVM package"}
            end
            local lists = path.join(os.projectdir(), "selfhost", "bundled", "symbols")
            -- what the startup object defines for a Windows program, which no list names; kept
            -- in step with selfhost/codegen/src/emit/startup.ens
            local startupDefines = {"__chkstk", "_fltused", "memcpy", "memmove", "memset",
                "memcmp", "snprintf"}
            local targets = {
                {triple = "x86_64-unknown-linux-gnu", list = "linux-x64.symbols", prefix = ""},
                {triple = "aarch64-unknown-linux-gnu", list = "linux-arm64.symbols", prefix = ""},
                {triple = "arm64-apple-macosx11.0", list = "macos-arm64.symbols", prefix = "_"},
                {triple = "x86_64-pc-windows-msvc", list = "windows-x64.symbols", prefix = "",
                    startup = startupDefines},
                {triple = "aarch64-pc-windows-msvc", list = "windows-arm64.symbols", prefix = "",
                    startup = startupDefines},
            }
            local failures = {}
            local audited = 0
            for _, target in ipairs(targets) do
                local provided = {}
                for line in (io.readfile(path.join(lists, target.list)) or ""):gmatch("[^\r\n]+") do
                    local kind, symbol = line:match("^(%S+) (%S+)")
                    if kind == "symbol" or kind == "weak-symbol" or kind == "object"
                            or kind == "tls" or kind == "data" then
                        provided[symbol] = true
                    end
                end
                for _, symbol in ipairs(target.startup or {}) do
                    provided[target.prefix .. symbol] = true
                end
                local folder = path.join(root, target.triple)
                local builds = {
                    {label = "the standard library", argv = {"build",
                        path.join(os.projectdir(), "libs", "std"), "--target", target.triple,
                        "--objects", path.join(folder, "std")}},
                    {label = "hello", argv = {"build", path.join(tests_dir, "hello.ens"),
                        "--target", target.triple, "--no-link", "--objects",
                        path.join(folder, "hello")}},
                }
                local compiled = true
                for _, build in ipairs(builds) do
                    if execMerged(host_exe, build.argv, log) ~= 0 then
                        table.insert(failures, string.format("%s did not compile for %s:\n%s",
                            build.label, target.triple, captured(log)))
                        compiled = false
                    end
                end
                if compiled then
                    local objects = {}
                    for _, file in ipairs(os.files(path.join(folder, "**"))) do
                        if file:endswith(".o") or file:endswith(".obj") then
                            table.insert(objects, file)
                        end
                    end
                    -- the names llvm-nm lists, one a line, with the file headers left out
                    local function listed(option)
                        local argv = {option, "--just-symbol-name"}
                        for _, object in ipairs(objects) do table.insert(argv, object) end
                        execMerged(nm, argv, log)
                        local found = {}
                        for line in captured(log):gmatch("[^\r\n]+") do
                            if not line:endswith(":") and #line > 0 then
                                found[line] = true
                            end
                        end
                        return found
                    end
                    local defined = listed("--defined-only")
                    local missing = {}
                    for symbol in pairs(listed("--undefined-only")) do
                        if not defined[symbol] and not provided[symbol] then
                            table.insert(missing, symbol)
                        end
                    end
                    table.sort(missing)
                    if #missing > 0 then
                        table.insert(failures, string.format("for %s, the objects call %d "
                            .. "symbol%s that neither the carried libraries nor the startup "
                            .. "object provide: %s", target.triple, #missing,
                            #missing == 1 and "" or "s", table.concat(missing, ", ")))
                    end
                    audited = audited + #objects
                end
            end
            if #failures > 0 then
                return {name = name, ok = false, short = failures[1]:match("^[^\n]*"),
                    full = name .. ": " .. table.concat(failures, "\n")}
            end
            return {name = name, ok = true,
                note = string.format("%d objects over %d targets", audited, #targets)}
        end

        local function run_standalone(job)
            local name = job.name
            local log = path.join(out_dir, name .. ".log")
            local libraries, why = loadedLibraries(host_exe, log)
            if not libraries then
                return {name = name, ok = false, short = why,
                    full = string.format("%s: %s\n%s", name, why, captured(log))}
            end
            local foreign = {}
            for _, library in ipairs(libraries) do
                if not isSystemLibrary(library) then
                    table.insert(foreign, library)
                end
            end
            if #foreign > 0 then
                return {name = name, ok = false, short = "loads " .. table.concat(foreign, ", "),
                    full = string.format("%s: '%s' loads libraries a machine without this build "
                        .. "does not have: %s", name, host_exe, table.concat(foreign, ", "))}
            end
            local alone_dir = path.join(os.projectdir(), "build", "standalone")
            os.tryrm(alone_dir)
            os.mkdir(alone_dir)
            local alone_exe = path.join(alone_dir, "ens" .. exe_suffix)
            os.cp(host_exe, alone_exe)
            local envs = {LIBRARY_PATH = "", LD_LIBRARY_PATH = "", DYLD_LIBRARY_PATH = "",
                DYLD_FALLBACK_LIBRARY_PATH = "", LIB = ""}
            if is_host("windows") then
                local root = os.getenv("SystemRoot") or "C:\\Windows"
                envs.PATH = path.join(root, "System32")
            else
                envs.PATH = "/usr/bin:/bin"
            end
            local rc = execMerged(alone_exe, {"version"}, log, {envs = envs, curdir = alone_dir})
            local printed = captured(log)
            if rc ~= 0 or not printed:startswith("ens ") then
                return {name = name, ok = false, short = "does not run on its own",
                    full = string.format("%s: '%s version' run from '%s' with no library paths "
                        .. "set exited %s:\n%s", name, alone_exe, alone_dir, tostring(rc),
                        printed)}
            end
            return {name = name, ok = true}
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

            local env = linkEnvironment()

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
            run({"--help"}, nil, 0, "Usage: ens <command>")
            run({"version"}, nil, 0, version_line)

            -- argv the command refuses: a problem the command line has, and one the options it
            -- settles into have
            run({"biuld", hello}, nil, 2, "ens: unknown command 'biuld'. Did you mean 'build'?")
            run({"build", "--output"}, nil, 2,
                "ens: option '--output' needs a value. Write '--output <file>' or '-o <file>'")
            run({"build", "-q", "-v"}, nil, 2,
                "ens: '--quiet' and '--verbose' cannot be used together. Remove one of them")
            -- every problem the command reports in its own words carries the 'ens: ' prefix, the
            -- one a compiler diagnostic never has because it names its own file and position
            run({"build", path.join(tests_dir, "no_such_place")}, nil, 2,
                "ens: '", "' does not exist")
            run({"build", path.join(root, "notes.txt")}, nil, 2, "not an Ens source file")
            run({"build"}, in_work, 2, "no ens.package manifest was found")

            -- a single file: named output, then the default name in the folder the command ran in
            local hello_exe = path.join(root, "hello_out.exe")
            run({"build", hello, "--output", hello_exe}, nil, 0, ": Built '")
            run_program(hello_exe, 0, "Hello, world!")
            run({"build", hello}, in_work, 0, ": Built '")
            run_program(path.join(work, "hello" .. exe_suffix), 0, "Hello, world!")

            -- a folder above a named program that cannot be made is reported with the option that
            -- names another
            run({"build", hello, "--output", path.join(root, "notes.txt", "sub", "hello.exe")}, nil,
                1, "ens: could not create the folder '", "notes.txt/sub' for the program: "
                .. "something in its path is a file, not a folder. Name a folder that can be "
                .. "written to with '--output'")

            -- every optimization level is a shipped configuration, so every one is run
            for _, level in ipairs({"-O0", "-O1", "-O2", "-O3"}) do
                local leveled = path.join(root, "hello" .. level .. ".exe")
                run({"build", hello, level, "--output", leveled}, nil, 0, ": Built '")
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

            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, ": Built '")
            local triple = path.filename(os.dirs(path.join(objects, ".ens", "*"))[1] or "none")
            expectFolders(objects, triple .. "/O2", "a default build")
            if #os.files(path.join(objects, ".ens", "*", "O2", "*" .. obj_suffix)) == 0 then
                table.insert(failures, "a build left no object files under the build root")
            end

            -- each object is assembled in a folder of the build's own and kept under a name that
            -- carries the digest of its bytes, so no build reads or replaces another's. What is
            -- asserted is that the staging folder is always taken away.
            local function expectNoStaging(base, after)
                local left = os.dirs(path.join(base, "staging-*"))
                if #left > 0 then
                    table.insert(failures, string.format("%s left %s behind", after,
                        path.filename(left[1])))
                end
            end
            expectNoStaging(path.join(objects, ".ens", triple, "O2"), "a build")
            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, ": Built '")
            expectNoStaging(path.join(objects, ".ens", triple, "O2"), "a build over its own objects")
            run_program(path.join(root, "objects.exe"), 0, "objects")

            -- an edited source is linked from its own new object
            io.writefile(path.join(objects, "src", "main.ens"),
                'main() -> int {\n    print("objects again");\n    return 0;\n}\n')
            run({"build", objects, "--output", path.join(root, "objects.exe")}, nil, 0, ": Built '")
            run_program(path.join(root, "objects.exe"), 0, "objects again")
            -- the edited object is kept under a name of its own, and its earlier version is cleared
            local mains = os.files(path.join(objects, ".ens", triple, "O2", "objects.main.*"
                .. obj_suffix))
            if #mains ~= 1 then
                table.insert(failures, string.format("the objects folder holds %d versions of "
                    .. "'objects.main'", #mains))
            end

            -- a library the linker cannot read can end the build from one of the linker's own
            -- threads, and the build still says why before the process ends, after everything
            -- it printed before the link
            if is_host("windows") then
                local damaged = path.join(root, "damaged")
                os.mkdir(path.join(damaged, "app", "src"))
                io.writefile(path.join(damaged, "app", "ens.package"), 'package demo.damaged {\n'
                    .. '    ens "0.1";\n\n'
                    .. '    native damaged {\n        windows "damaged";\n    }\n}\n')
                io.writefile(path.join(damaged, "app", "src", "main.ens"),
                    'main() -> int {\n    return 0;\n}\n')
                -- an object for this machine's processor whose one relocation names the aux
                -- record of its section symbol, which the linker reads as a symbol whose name lies
                -- past the string table. It reads that name only while writing the program, on
                -- its thread pool. The machine is the first word, so the linker gets as far as
                -- that name rather than refusing the object for another processor's.
                local machine = os.arch() == "arm64" and "64aa" or "6486"
                local object = table.concat({
                    machine .. "010000000000560000000300000000000000",
                    "2e746578740000000000000000000000100000003c000000",
                    "4c000000000000000100000020005060",
                    "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3",
                    "00000000010000000100",
                    "2e7465787400000000000000010000000301",
                    "00000000ffffffff00000000000000000000",
                    "737461727400000000000000010020000200",
                    "04000000",
                })
                local library = io.open(path.join(damaged, "damaged.lib"), "wb")
                library:write((object:gsub("%x%x", function (pair)
                    return string.char(tonumber(pair, 16))
                end)))
                library:close()
                run({"build", "app", "--output", path.join(damaged, "app.exe"), "-v"},
                    {curdir = damaged}, 1, "damaged: Reading '",
                    "damaged/app.exe' stopped the build; the linker reported:",
                    "lld-link: error: string table empty", "!.ens-link")
                -- the folder that build was linking in is taken back by the next build beside it
                os.rm(path.join(damaged, "damaged.lib"))
                run({"build", "app", "--output", path.join(damaged, "app.exe")}, {curdir = damaged},
                    1, "could not open 'damaged.lib'")
                if #os.dirs(path.join(damaged, ".ens-link-*")) > 0 then
                    table.insert(failures, "a build left the folder of the build the linker ended "
                        .. "beside the program")
                end
                expectNoStaging(path.join(damaged, "app", ".ens", triple, "O2"),
                    "a build after the one the linker ended")
            end

            -- and nowhere else: not beside the executable, and not in the folder it was run from
            for _, elsewhere in ipairs({root, work}) do
                if #os.files(path.join(elsewhere, "*" .. obj_suffix)) > 0 then
                    table.insert(failures, string.format("object files were left in %s", elsewhere))
                end
            end

            -- the folder says to ignore everything under it, so nothing has to be added to a
            -- .gitignore anywhere and nobody has to remember it
            local ignored = io.readfile(path.join(objects, ".ens", ".gitignore")) or ""
            if ignored:gsub("%s+", "") ~= "*" then
                table.insert(failures, string.format("the artifacts folder ignores %q", ignored))
            end

            -- quiet says nothing on success, and verbose says what it did.
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
                "Emitted 2 object files from 9 modules")

            -- an application package is named after its package; a library keeps no artifact and
            -- refuses an output, while keeping its object files the way a program keeps its own.
            -- The library's build root is in the fixture tree, so what an earlier run left there
            -- goes first and the assertion is about this run.
            run({"build", path.join(tests_dir, "pkg_import_main")}, in_work, 0, ": Built '")
            run_program(path.join(work, "main" .. exe_suffix), 0, "Hello, Ada! [acme.tools]")
            local library_root = path.join(tests_dir, "pkg_import_dep")
            os.tryrm(path.join(library_root, ".ens"))
            run({"build", library_root}, in_work, 0, "Compiled 12 modules as a library")
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
                path.join(root, "tools.exe")}, nil, 2, "so it is a library and there is no "
                .. "executable to write. Remove '--output' to compile it as a library")

            -- a build that links nothing leaves its object files, for a linker on another
            -- machine, and writes no program
            local unlinked = path.join(root, "unlinked")
            os.tryrm(path.join(work, "hello" .. exe_suffix))
            run({"build", hello, "--no-link", "--objects", unlinked}, in_work, 0,
                "hello: Emitted 2 object files from 9 modules into '", "', and linked nothing")
            if #os.files(path.join(unlinked, "*" .. obj_suffix)) ~= 2 then
                table.insert(failures, string.format("a build that links nothing left %d object "
                    .. "files", #os.files(path.join(unlinked, "*" .. obj_suffix))))
            end
            if os.isfile(path.join(work, "hello" .. exe_suffix)) then
                table.insert(failures, "a build that links nothing wrote a program")
            end
            expectNoStaging(unlinked, "a build into a named objects folder")

            -- a stripped build writes a program with no symbol table, which runs the same
            local stripped_exe = path.join(root, "stripped", "hello" .. exe_suffix)
            run({"build", hello, "--strip", "--output", stripped_exe}, nil, 0, ": Built '")
            run_program(stripped_exe, 0, "Hello, world!")
            local nm = llvmTool("llvm-nm")
            if not nm then
                table.insert(failures, "no llvm-nm in the LLVM package to read the stripped "
                    .. "program with")
            elseif os.isfile(stripped_exe) then
                execMerged(nm, {"--defined-only", "--just-symbol-name", "--quiet", stripped_exe},
                    log)
                if #captured(log) > 0 then
                    table.insert(failures, "a stripped build left symbols in its program:\n"
                        .. captured(log))
                end
            end

            -- check writes nothing and reports what it found
            run({"check", hello}, nil, 0, "nothing to report")
            run({"check", path.join(tests_dir, "pkg_import_dep")}, nil, 0, "nothing to report")

            -- a package's check reads its tests as well, so a problem inside a test is reported and
            -- a fix it offers never breaks them: the test importing 'Registry' keeps 'public' on it
            local tested = path.join(root, "tested")
            os.mkdir(path.join(tested, "src"))
            os.mkdir(path.join(tested, "tests"))
            io.writefile(path.join(tested, "ens.package"),
                'package demo.registry {\n    ens "0.1";\n}\n')
            io.writefile(path.join(tested, "src", "main.ens"), 'main() {\n}\n')
            io.writefile(path.join(tested, "src", "lib.ens"), 'class Secret {\n}\n\n'
                .. 'public class Registry {\n    public constructor();\n\n'
                .. '    protected keep(Secret secret) {\n    }\n}\n')
            io.writefile(path.join(tested, "tests", "registry_test.ens"),
                'import Registry from lib;\n\ntest "a registry can be made" {\n'
                .. '    let registry = new Registry();\n    int wrong = "text";\n}\n')
            run({"check", tested}, nil, 1, "registry_test.ens:5:17: error: Variable "
                .. "'wrong' has type 'int', but its value has type 'string'.", "registry: 2 problems")

            -- the standard library named as the target is checked once, as the library itself
            local std_folder = path.join(os.projectdir(), "libs", "std")
            run({"check", std_folder, "--stdlib", path.join(os.projectdir(), "libs")}, nil, 0,
                "std: ", "nothing to report", "!(from std.")
            run({"check", std_folder, "--stdlib", root}, nil, 2, "is the package 'std', which is "
                .. "the standard library, so this command uses it as the standard library. "
                .. "'--stdlib' names a different folder, '",
                "Remove '--stdlib', or write '--stdlib ")

            -- a folder named as the standard library that holds none is reported once, by the
            -- command, rather than as every module the build then cannot find
            local hello_file = path.join(tests_dir, "hello.ens")
            local empty_library = path.join(root, "emptylibrary")
            os.mkdir(empty_library)
            local variable_env = {}
            for key, value in pairs(env) do variable_env[key] = value end
            variable_env.ENS_STDLIB = empty_library
            run({"check", hello_file}, {envs = variable_env}, 2, "ens: 'ENS_STDLIB' names '",
                "', which does not contain the standard library: there is no 'std/ens.package' "
                .. "in it. Set 'ENS_STDLIB' to the folder that contains 'std/', or unset it",
                "!Cannot find standard library module", "!problem")

            -- with no library named, a build uses the copy ens carries, written into the cache
            -- once under the digest of what it holds and read from there by every later build
            local carried_env = {}
            for key, value in pairs(env) do carried_env[key] = value end
            -- the run's environment is merged into what a command gets, so a variable is cleared
            -- by setting it empty, which ens reads as not set
            carried_env.ENS_STDLIB = ""
            local carried_cache = path.join(root, "carriedcache")
            carried_env.ENS_CACHE = carried_cache
            run({"check", hello_file}, {envs = carried_env}, 0, "nothing to report")
            run({"build", hello_file, "--output", path.join(root, "carried" .. exe_suffix)},
                {envs = carried_env}, 0)
            run_program(path.join(root, "carried" .. exe_suffix), 0, "Hello, world!")
            -- a note into the library names the file in the cache, which is there to be opened
            local into_library = path.join(root, "intolibrary.ens")
            io.writefile(into_library, "import @std.testing;\n\nstruct Key {\n"
                .. "    () -> int make;\n}\n\nmain() -> int throws {\n"
                .. "    Key first = {make: () -> 1};\n    try testing.assertEqual(first, first);\n"
                .. "    return 0;\n}\n")
            local _, noted = run({"check", into_library}, {envs = carried_env}, 1,
                "note: 'assertEqual<T>' interpolates a value of type 'T' here")
            local noted_file = noted:match("([^\r\n]-testing%.ens):%d+:%d+: note:")
            local function slashed(p) return (p:gsub("\\", "/")) end
            if not noted_file or not slashed(noted_file):find(slashed(carried_cache), 1, true)
                    or not os.isfile(noted_file) then
                table.insert(failures, string.format("the note into the carried library does not "
                    .. "name a file in '%s':\n%s", carried_cache, noted))
            end
            local blocked_cache = path.join(root, "blockedcache")
            io.writefile(blocked_cache, "a file where the cache would be")
            -- a program for this machine links against the system libraries ens carries, which
            -- go into the cache too, so a cache that cannot be written stops the link and says why
            local system = is_host("windows") and "Windows" or is_host("linux") and "Linux"
                or os.arch() == "arm64" and "macOS" or nil
            if system then
                local linking_env = {}
                for key, value in pairs(env) do linking_env[key] = value end
                linking_env.ENS_CACHE = blocked_cache
                run({"build", hello_file, "--output", path.join(root, "blocked" .. exe_suffix)},
                    {envs = linking_env}, 1, "a " .. system .. " program is linked against lists "
                    .. "of what " .. system .. "'s system libraries provide, which ens writes into "
                    .. "its cache, and the lists could not be put there: could not create the "
                    .. "directory '", "': something in the path is a file, not a directory. Set "
                    .. "'ENS_CACHE' to a folder that can be written to")
            end
            carried_env.ENS_CACHE = ""
            carried_env.HOME = ""
            carried_env.USERPROFILE = ""
            local user_variable = is_host("windows") and "USERPROFILE" or "HOME"
            run({"check", hello_file}, {envs = carried_env}, 2, "ens: there is no cache folder to "
                .. "write the standard library into: neither 'ENS_CACHE' nor '" .. user_variable
                .. "' is set. Set 'ENS_CACHE' to a folder that can be written to, or pass "
                .. "'--stdlib <folder>' naming the folder that contains 'std/'", "!problem")

            -- a package of the user's own named 'std' is refused for the name the standard
            -- library reserves, at the name
            local user_std = path.join(root, "userstd", "std")
            os.mkdir(path.join(user_std, "src"))
            io.writefile(path.join(user_std, "ens.package"), 'package std {\n    ens "0.1";\n}\n')
            io.writefile(path.join(user_std, "src", "tools.ens"),
                'export answer() -> int {\n    return 1;\n}\n')
            run({"check", user_std, "--stdlib", path.join(os.projectdir(), "libs")}, nil, 1,
                "ens.package:1:9: error: The package name 'std' is reserved for the standard "
                .. "library, which programs import as '@std'. Rename this package, as in "
                .. "'package acme.tools'.", "!is the package 'std', which is the standard library")

            -- a workspace root builds every member, the library before the application that
            -- depends on it even though the manifest lists the application first
            local workspace = path.join(tests_dir, "cli_workspace")
            run({"build", workspace}, in_work, 0, "[1/2] demo.lib: Compiled 9 modules as a "
                .. "library", "[2/2] demo.app: Built '")
            run({"build", workspace, "--output", path.join(root, "x.exe")}, nil, 2,
                "ens: '--output' names one executable, but '", "' is a workspace that builds one "
                .. "for each member. Name a member's folder to build only that member, or remove "
                .. "'--output' so each member is named after its package")

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
            run({"build", split}, nil, 2, "are written for different versions of Ens")

            -- a workspace whose own manifest has problems reports each of them and reads no member
            local misdeclared = path.join(root, "misdeclared")
            os.mkdir(path.join(misdeclared, "app", "src"))
            io.writefile(path.join(misdeclared, "ens.package"),
                'workspace {\n    member "app"\n    version "1.0";\n}\n')
            io.writefile(path.join(misdeclared, "app", "ens.package"),
                'package misdeclared.app {\n    ens "0.1";\n}\n')
            io.writefile(path.join(misdeclared, "app", "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", misdeclared}, nil, 1,
                "ens.package:2:17: error: Expected ';' after the member declaration.",
                "ens.package:3:5: error: Expected a 'member' line inside the workspace "
                .. "declaration. For example, write 'member \"frontend\";'.",
                "ens: misdeclared: 2 problems in its ens.package, so none of its members was read",
                "!misdeclared.app")

            -- a source folder reached through a symbolic link is compiled, which is how a checkout
            -- that shares one folder between two packages builds at all. The link is made with the
            -- system's own command, and Windows makes a junction, which needs no privilege a test
            -- run may lack; a system that refuses to make one leaves the case unasserted rather
            -- than failing the run.
            local linked_root = path.join(root, "linked")
            os.mkdir(path.join(linked_root, "src"))
            os.mkdir(path.join(linked_root, "shared", "deeper"))
            io.writefile(path.join(linked_root, "ens.package"),
                'package demo.linked {\n    ens "0.1";\n}\n')
            io.writefile(path.join(linked_root, "src", "main.ens"),
                'import shared.deeper.greet;\n\nmain() -> int {\n'
                .. '    print(greet.text());\n    return 0;\n}\n')
            io.writefile(path.join(linked_root, "shared", "deeper", "greet.ens"),
                'export text() -> string {\n    return "through the link";\n}\n')
            local link_target = path.join(linked_root, "shared")
            local link_name = path.join(linked_root, "src", "shared")
            local link_made
            if is_host("windows") then
                link_made = os.execv("cmd", {"/c", "mklink", "/J", link_name, link_target},
                    {stdout = log, stderr = log}) == 0
            else
                link_made = os.execv("ln", {"-s", link_target, link_name},
                    {stdout = log, stderr = log}) == 0
            end
            if link_made then
                run({"build", linked_root, "--output", path.join(root, "linked.exe")}, nil, 0,
                    ": Built '")
                run_program(path.join(root, "linked.exe"), 0, "through the link")
            end

            -- the link reads none of the variables meant for Microsoft's link.exe
            if is_host("windows") then
                local linking = {}
                for key, value in pairs(env) do linking[key] = value end
                linking.LINK = "/threads:0"
                linking._LINK_ = "/threads:0"
                run({"build", hello, "--output", path.join(root, "unaffected.exe")},
                    {envs = linking}, 0, ": Built '")
                run_program(path.join(root, "unaffected.exe"), 0, "Hello, world!")
            end

            -- the hidden syntax tools
            run({"cst-analyze", hello}, nil, 0)
            run({"cst-analyze", path.join(root, "broken.ens")}, nil, 1, "broken.ens:")
            -- one problem said once: the failure names the operation and the path, and the command
            -- adds its prefix and nothing else
            run({"cst-dump", path.join(root, "gone.ens")}, nil, 2, "ens: could not open '",
                "gone.ens': nothing is there", "!could not read")

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

            local base = linkEnvironment()
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
                ["src/main.ens"] = 'import @std.environment;\n\nmain() -> int {\n'
                    .. '    string[] argv = environment.arguments();\n'
                    .. '    for (long i = 0; i < argv.length; i++) {\n'
                    .. '        print("argument {i + 1}: {argv[i]}");\n    }\n'
                    .. '    if (argv.length > 0) {\n'
                    .. '        return parsed(argv[0]);\n    }\n    return 0;\n}\n\n'
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
            run({"run", echo, "-v"}, nil, 0, "Building 'echo' for", "Running '")

            -- a target with no main() is a usage problem, not a build failure
            run({"run", path.join(tests_dir, "pkg_import_dep")}, nil, 2, "' cannot be run, because "
                .. "its main module does not define main().", "Add a main() function there")

            -- a program that does not compile fails the work rather than the command line
            local broken = path.join(root, "broken")
            writePackage(broken, 'package demo.broken {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'main() -> int {\n    return missing();\n}\n',
            })
            run({"run", broken}, nil, 1, "' was not built, so it was not run")
            assertTempEmpty("a failed ens run")

            -- a syntax error that may hide main() is the build's to report, not a missing main()
            local hidden = path.join(root, "hidden")
            writePackage(hidden, 'package demo.hidden {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'attempt() {\n    if (true) {\n\n  main() -> int {\n'
                    .. '    return 0;\n}\n',
            })
            run({"run", hidden}, nil, 1, "Expected '}' to close the block that starts on line 2.",
                "' was not built, so it was not run", "!does not define main()")

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
            run({"run", none}, nil, 2, "' has no program to run: no member's 'src/main.ens' defines "
                .. "main(). Add main() to the 'src/main.ens' of the member to run")

            -- the one member whose syntax error may hide main() is built, and the build reports it
            local unsure = path.join(root, "unsure")
            os.mkdir(unsure)
            io.writefile(path.join(unsure, "ens.package"),
                'workspace {\n    member "app";\n    member "lib";\n}\n')
            writePackage(path.join(unsure, "app"), 'package unsure.app {\n    ens "0.1";\n}\n', {
                ["src/main.ens"] = 'attempt() {\n    if (true) {\n\n  main() -> int {\n'
                    .. '    return 0;\n}\n',
            })
            writePackage(path.join(unsure, "lib"), 'package unsure.lib {\n    ens "0.1";\n}\n', {
                ["src/greet.ens"] = 'export greet() -> string {\n    return "hi";\n}\n',
            })
            run({"run", unsure}, nil, 1, "Expected '}' to close the block that starts on line 2.",
                "'unsure.app' was not built, so it was not run", "!has no program to run")

            -- two members whose syntax errors may each hide main() are not told apart
            local twounsure = path.join(root, "twounsure")
            os.mkdir(twounsure)
            io.writefile(path.join(twounsure, "ens.package"),
                'workspace {\n    member "app";\n    member "tool";\n}\n')
            for _, member in ipairs({"app", "tool"}) do
                writePackage(path.join(twounsure, member),
                    'package twounsure.' .. member .. ' {\n    ens "0.1";\n}\n', {
                    ["src/main.ens"] = 'attempt() {\n    if (true) {\n\n  main() -> int {\n'
                        .. '    return 0;\n}\n',
                })
            end
            run({"run", "."}, {curdir = twounsure}, 2, "the 'src/main.ens' of 'twounsure.app' and "
                .. "'twounsure.tool' in the workspace at '", "' have syntax errors, so either may be "
                .. "the program to run. Name the folder of the member to run, as in 'ens run app', "
                .. "and the build reports its errors", "!has no program to run")

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

            -- a package whose tests pass, and one whose tests fail
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
            local failing = path.join(root, "failing")
            writePackage(failing, 'package demo.failing {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export twice(long n) -> long {\n    return n * 3;\n}\n',
                ["tests/math_test.ens"] = 'import @std.testing;\nimport math;\n\n'
                    .. 'test "twice a small number" {\n'
                    .. '    try testing.assertEqual(math.twice(2), 4L);\n}\n',
            })
            run({"test", failing}, nil, 1, "FAIL twice a small number", "0/1 tests passed")

            -- a folder holding no tests, and a '--tests' folder that is not there
            local bare = path.join(root, "bare")
            writePackage(bare, 'package demo.bare {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'export one() -> long {\n    return 1;\n}\n',
            })
            run({"test", bare}, nil, 0, "There are no tests in")
            run({"test", suite, "--tests", path.join(root, "nowhere")}, nil, 2, "is not a folder")

            -- '--tests' names where the tests of one target live
            run({"test", suite, "--tests", path.join(suite, "tests")}, nil, 0, "2/2 tests passed")

            -- the folder '--tests' names is part of the package it tests wherever it sits, so a
            -- test there reaches the package's 'public' members the way one inside it does
            local outside = path.join(root, "outside")
            writePackage(outside, 'package demo.outside {\n    ens "0.1";\n}\n', {
                ["src/math.ens"] = 'public tripled(long n) -> long {\n    return n * 3;\n}\n',
            })
            local elsewhere = path.join(root, "elsewheretests")
            os.mkdir(elsewhere)
            io.writefile(path.join(elsewhere, "math_test.ens"),
                'import @std.testing;\nimport math;\n\n'
                .. 'test "tripling a small number" {\n'
                .. '    try testing.assertEqual(math.tripled(2), 6L);\n}\n')
            run({"test", outside, "--tests", elsewhere}, nil, 0, "PASS tripling a small number",
                "1/1 tests passed")

            -- a file is not a folder of tests, and a workspace root tests its members
            run({"test", path.join(tests_dir, "hello.ens")}, nil, 2, "' is one file, and 'ens test' "
                .. "reads a folder of test files. Name the folder the tests are in, or the package "
                .. "they belong to")
            run({"test", one, "--tests", path.join(suite, "tests")}, nil, 2,
                "'--tests' names one folder of tests")
            run({"test", one}, nil, 0, "There are no tests in")

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
            run({"test", suites}, nil, 2, "the tests were not built, so none of them ran",
                "1/2 tests passed across 2 members; 'suite.beta' did not finish")
            assertTempEmpty("a workspace member whose tests did not compile")

            -- a member with no tests of its own is not one of the members the total counts
            os.rm(beta_tests)
            run({"test", suites}, nil, 0, "There are no tests in",
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

            local env = linkEnvironment()

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

            -- with nothing redirected, there is nothing to list
            run({"override", "list"}, in_ws, 0, "No packages are overridden")

            -- add writes the file, list says the folder works, and the build resolves through it
            run({"override", "add", "acme.json", "../json"}, in_ws, 0,
                "Added the override for package 'acme.json': '../json'")
            run({"override", "list"}, in_ws, 0, "'acme.json' -> '../json'", "!not usable")
            -- the build says what it is taking from the override, because nothing in the program
            -- does, and a check says it too
            local notice = "Using the override for package 'acme.json': '"
                .. path.absolute(path.join(root, "json")):gsub("\\", "/") .. "'"
            run({"build"}, in_ws, 0, "demo.app: Built '", notice)
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
            local _, said = run({"build"}, in_ws, 0, "demo.app: Built '",
                "demo.tool: Compiled 10 modules as a library")
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

            -- an override already there is replaced
            io.writefile(overrides_file, '// local checkouts\noverrides {\n'
                .. '    override acme.json "../wrong";  // wrong on purpose\n'
                .. '\n'
                .. '    override beta.tools "../missing";\n}\n')
            run({"override", "add", "acme.json", "../json"}, in_ws, 0,
                "Replaced the override for package 'acme.json': now '../json'")

            -- list reports a valid target and an invalid one, with the reason for the invalid one
            local _, listed = run({"override", "list"}, in_ws, 0, "'acme.json' -> '../json'",
                "'beta.tools' -> '../missing'", "not usable",
                "there is no ens.package manifest at")
            if listed:find("'acme.json' -> '../json' (", 1, true) then
                table.insert(failures, string.format("a usable override was reported with a "
                    .. "reason:\n%s", listed))
            end

            run({"override", "remove", "beta.tools"}, in_ws, 0,
                "Removed the override for package 'beta.tools'")

            -- a folder is read from where the command ran and written down relative to the
            -- workspace, so running from a subfolder still records one path
            os.mkdir(path.join(ws_dir, "notes"))
            run({"override", "add", "acme.json", "../../json"},
                {curdir = path.join(ws_dir, "notes"), envs = env}, 0, "now '../json'")

            -- state and usage problems, each one saying what to do instead
            run({"override", "remove", "nope.pkg"}, in_ws, 1,
                "package 'nope.pkg' is not overridden in '", "'. Run 'ens override list' to see the "
                    .. "overridden packages")
            run({"override", "add", "not/a/name!", "../json"}, in_ws, 2, "is not a package name")
            run({"override", "list"}, {curdir = root, envs = env}, 2,
                "no ens.package manifest was found")

            -- a file that does not parse is one problem sentence of the command's own, with the
            -- reading's own diagnostic under it at the line and column it is about
            local unreadable = path.join(root, "unreadable")
            os.mkdir(unreadable)
            io.writefile(path.join(unreadable, "ens.package"),
                'package demo.unreadable {\n    ens "0.1";\n}\n')
            io.writefile(path.join(unreadable, "ens.overrides"), 'overrides {\n    nonsense\n')
            run({"override", "list"}, {curdir = unreadable, envs = env}, 1,
                "ens: '", "ens.overrides' has the errors listed below. Fix them by hand before "
                    .. "editing the file with 'ens override'",
                "ens.overrides:2:5: error: ")

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

            local env = linkEnvironment()

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

            -- the repositories: a package with two tagged versions (one behind an annotated 'v'
            -- tag), a package raising another's version transitively, a
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
            -- transitively raised beta.utils 1.1. The tag listing is read, never shown, and a
            -- verbose run says which step each wait is on.
            run({"build", ".", "-v"}, in_app, 0,
                "Choosing versions for 3 packages",
                "Listing the tags at '" .. url_json .. "'",
                "Fetching the tag '1.0' of 'alex.json' from '" .. url_json .. "'",
                "Fetched 'alex.json' 1.0 from '" .. url_json .. "' (tag '1.0')",
                "Fetched 'acme.tools' 1.0",
                "Fetched 'beta.utils' 1.1",
                "Updated ens.lock: added 'acme.tools' 1.0, added 'alex.json' 1.0, "
                    .. "added 'beta.utils' 1.1",
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

            -- a single file of the package is built under the package's manifest, so it fetches
            -- the package's git dependencies into a cache of its own, as the package build did,
            -- and so does a check of it
            local single_cache = path.join(root, "single-cache")
            local in_app_single = {curdir = app, envs = withCache(single_cache)}
            local single = path.join(app, "single" .. exe_suffix)
            run({"build", path.join("src", "main.ens"), "--output", single}, in_app_single, 0,
                "!only a build downloads")
            run_program(single, 0, "json 1.0 | tools(utils 1.1)")
            run({"check", path.join("src", "main.ens")}, in_app_single, 0, "nothing to report",
                "!only a build downloads")
            if lock_text() ~= locked then
                table.insert(failures, "building a single file of the package changed its lock\n"
                    .. lock_text())
            end

            -- a locked build asks the network nothing: the repositories are gone and it still
            -- reproduces, with the lock untouched
            os.mv(repos, repos .. ".away")
            run({"build", "."}, in_app, 0, "gitapp: Built '", "!Fetched", "!Updated ens.lock")
            if lock_text() ~= locked then
                table.insert(failures, "a build that fetched nothing rewrote ens.lock")
            end
            run({"build", ".", "--offline"}, in_app, 0, "gitapp: Built '", "!Fetched")
            run({"check", ".", "--offline", "--locked"}, in_app, 0, "nothing to report")

            -- a cold cache under --offline fails by name, and says how to fix it
            run({"build", ".", "--offline"}, {curdir = app, envs = withCache(cold)}, 1,
                "1.0 is not in the cache, and '--offline' does not allow fetching it from '",
                "'. Run the command once without '--offline'")
            os.mv(repos .. ".away", repos)

            -- --locked turns a pending change into an error, names what would change, and leaves
            -- the lock exactly as it was
            write_app("1.1")
            run({"build", ".", "--locked"}, in_app, 1,
                "ens.lock no longer matches what this build requires",
                "updated 'alex.json' from 1.0 to 1.1", "'--locked' does not allow it to change. Run "
                    .. "the command without '--locked' to update the lock")
            if lock_text() ~= locked then
                table.insert(failures, "--locked changed ens.lock")
            end

            -- the update fetches the raised version through its annotated 'v' tag and says what
            -- changed and nothing more
            run({"build", "."}, in_app, 0,
                "Fetched 'alex.json' 1.1 from '" .. url_json .. "' (tag 'v1.1')",
                "Updated ens.lock: updated 'alex.json' from 1.0 to 1.1", "!added 'beta.utils'")
            run_program(path.join(app, "gitapp" .. exe_suffix), 0, "json 1.1 | tools(utils 1.1)")
            if not lock_text():find("package alex.json 1.1", 1, true) then
                table.insert(failures, "ens.lock does not record the raised version")
            end

            -- a version nothing is tagged for names both spellings and the way in for unreleased
            -- work
            write_app("9.9")
            run({"build", "."}, in_app, 1,
                "package 'alex.json' has no tag '9.9' or 'v9.9'",
                "Tag the release, or build against unreleased work in a folder with "
                    .. "'ens override add alex.json <folder>'")
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
                "package 'beta.utils' is required at two major versions: '",
                "requires '1.1'", "requires '2.0'",
                "A build uses one major version of a package, so change one of the two "
                    .. "requirements to match the other")

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
                "Fetched 'ws.core' 1.0")
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
                "contains a workspace whose members are 'ws.core' and 'ws.extra', and none of "
                    .. "them is 'ws.absent'. Check the dependency's name and its URL")

            -- a package using submodules is refused, because a package is used as it was fetched
            local using_sub = path.join(root, "usingsub")
            os.mkdir(path.join(using_sub, "src"))
            write_package(using_sub, "demo.subapp",
                {'dependency sub.pkg "1.0" from "' .. url_sub .. '";'})
            io.writefile(path.join(using_sub, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = using_sub, envs = withCache(cache)}, 1,
                "uses git submodules, and a package has to contain all of its own files. Declare "
                    .. "what the submodule contains as a dependency of")

            -- a repository that is not there is reported with git's reason and what to check
            local using_ghost = path.join(root, "usingghost")
            os.mkdir(path.join(using_ghost, "src"))
            write_package(using_ghost, "demo.ghostapp",
                {'dependency ghost.lib "1.0" from "' .. url_of(path.join(repos, "ghost")) .. '";'})
            io.writefile(path.join(using_ghost, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = using_ghost, envs = withCache(cache)}, 1,
                "could not list the tags of package 'ghost.lib' at '",
                ". Check that the URL is right and that the repository exists", "!exists..")

            -- a tag that moved is caught against the lock, naming both digests
            local using_retag = path.join(root, "usingretag")
            os.mkdir(path.join(using_retag, "src"))
            write_package(using_retag, "demo.retag",
                {'dependency rt.pkg "1.0" from "' .. url_retag .. '";'})
            io.writefile(path.join(using_retag, "src", "main.ens"),
                'import @rt.pkg.thing;\n\nmain() -> int {\n    print(thing.tag());\n'
                .. '    return 0;\n}\n')
            local in_retag = {curdir = using_retag, envs = withCache(cache)}
            run({"build", "."}, in_retag, 0, "Fetched 'rt.pkg' 1.0")
            run_program(path.join(using_retag, "retag" .. exe_suffix), 0, "retag 1")
            io.writefile(path.join(retag_dir, "src", "thing.ens"),
                'export tag() -> string {\n    return "retag 2";\n}\n')
            commit_all(retag_dir, "moved")
            git(retag_dir, "tag", "-f", "1.0")
            os.tryrm(path.join(cache, "trees"))
            run({"build", "."}, in_retag, 1,
                "are not the files ens.lock records: the lock has 'sha256:",
                "and the fetched files have 'sha256:", "may have moved since the lock was written. "
                    .. "If the new files are the right ones, delete ens.lock and build again to "
                    .. "record them")

            -- losing the last git dependency removes the lock, and --locked refuses that too
            write_package(using_retag, "demo.retag")
            io.writefile(path.join(using_retag, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", ".", "--locked"}, in_retag, 1,
                "this build no longer fetches any git package or prebuilt library, so ens.lock "
                    .. "would be removed, and '--locked' does not allow that. Run the command "
                    .. "without '--locked' to remove it")
            run({"build", "."}, in_retag, 0,
                "Removed ens.lock: this build no longer fetches any git package or prebuilt library")
            if os.isfile(path.join(using_retag, "ens.lock")) then
                table.insert(failures, "ens.lock survived losing its last git dependency")
            end

            -- a lock nobody may edit by hand says so rather than being read as one
            io.writefile(lock_file, "hand written\n")
            run({"build", "."}, in_app, 1, "is written by the build",
                "delete it and build again")

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

            local env = linkEnvironment()

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
            run({"build", "."}, in_app, 0, "prebuilt: Built '")
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
            run({"build", ".", "--locked"}, in_app, 0, "prebuilt: Built '",
                "!ens.lock no longer matches")
            if ((io.readfile(only_lock) or ""):gsub("\r\n", "\n")) ~= locked_text then
                table.insert(failures, "a second artifact-only build rewrote ens.lock")
            end

            -- a cached library needs no network, even with the file it came from gone
            os.mv(files, files .. ".away")
            run({"build", ".", "--offline"}, in_app, 0, "prebuilt: Built '")
            run({"build", ".", "--offline"}, {curdir = app, envs = withCache(cold)}, 1,
                "the prebuilt library for native 'extras' is not in the cache, and '--offline' "
                    .. "does not allow downloading it from '",
                "'. Run the command once without '--offline'")
            os.mv(files .. ".away", files)

            -- rebinding a prebuilt library is a lock change, so '--locked' refuses it by name
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
                "Updated ens.lock: added the prebuilt libraries of this build itself")
            write_lifecycle(binding("renamed", good))
            run({"build", ".", "--locked"}, in_lifecycle, 1,
                "ens.lock no longer matches what this build requires",
                "changed the prebuilt libraries of this build itself",
                "'--locked' does not allow it to change")

            -- a digest that does not match is refused, naming both digests, and nothing is cached
            local bad = path.join(root, "bad")
            os.mkdir(path.join(bad, "src"))
            io.writefile(path.join(bad, "ens.package"),
                "package demo.badhash {\n" .. '    ens "0.1";\n\n'
                .. binding("extras", wrong) .. "}\n")
            io.writefile(path.join(bad, "src", "main.ens"),
                'main() -> int {\n    return 0;\n}\n')
            run({"build", "."}, {curdir = bad, envs = withCache(cache)}, 1,
                "has the hash '" .. good .. "', but the manifest declares '" .. wrong .. "', so it "
                    .. "is not the file the manifest names. If the library changed on purpose, "
                    .. "write the new hash in the manifest")
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
                "capitals: Built '")
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
                "Fetched 'art.dep' 1.0", "Updated ens.lock: added 'art.dep' 1.0")
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
            run({"build", "."}, {curdir = ws, envs = withCache(cache)}, 0,
                "demo.member: Built '")
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
                "demo.member: Built '", "!ens.lock no longer matches")
            if ((io.readfile(path.join(ws, "ens.lock")) or ""):gsub("\r\n", "\n")) ~= ws_lock then
                table.insert(failures, "a second build rewrote the member's lock")
            end

            if #failures == 0 then
                return {name = name, ok = true}
            end
            return {name = name, ok = false,
                short = string.format("%d CLI assertion(s) failed", #failures),
                full = string.format("%s:\n%s", name, table.concat(failures, "\n"))}
        end

        -- version delegation: `ens` handing a whole invocation to another toolchain. Copies of the
        -- built command are installed as toolchains 9.9.0 and 9.9.2 in a scratch folder, which is
        -- what makes the mechanism observable while only one real version exists - each copy is
        -- the tree's own version, so it would hand the work straight back if the loop guard let it.
        local function run_cli_toolchain(job)
            local name = job.name
            local root = path.join(os.projectdir(), "build", "cli", "enstoolchain")
            os.tryrm(root)
            os.mkdir(root)
            local log = path.join(out_dir, name .. ".log")
            local failures = {}

            local env = linkEnvironment()

            local toolchains = path.join(root, "toolchains")
            local installed_dir = path.join(toolchains, "9.9.2")
            local installed_exe = path.join(installed_dir, "ens" .. exe_suffix)
            os.mkdir(installed_dir)
            os.cp(host_exe, installed_exe)
            local lower_patch_exe = path.join(toolchains, "9.9.0", "ens" .. exe_suffix)
            os.mkdir(path.directory(lower_patch_exe))
            os.cp(host_exe, lower_patch_exe)
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
            -- refusal of --output is a usage exit code to propagate; packages written for an older
            -- version than this one and for a newer one than any installed; and one that does not
            -- compile, whose failure is the other exit code.
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
            local lower_patch = slashed(lower_patch_exe)
            local declared_newer = "declares 'ens \"9.9\";', so it needs Ens 9.9 or later, and this "
                .. "toolchain is Ens "

            -- the hop: the package is built by the highest installed patch of the version it is
            -- written for, and the command line reaches it exactly as it was written here, spaces
            -- and all
            local spaced_out = path.join(root, "spaced out.exe")
            run(host_exe, {"build", pkg, "--output", spaced_out, "-v"}, chains, 0,
                slashed(pkg) .. "' " .. declared_newer, "Ens 9.9.2 at '" .. hopped_to, ": Built '",
                "!" .. lower_patch)
            run_program(spaced_out, 0, "built by a delegate")

            -- the exit code of the toolchain that did the work is this command's own
            run(host_exe, {"check", broken, "-v"}, chains, 1, hopped_to,
                "Undefined function 'missing'")
            run(host_exe, {"build", library, "--output", path.join(root, "library.exe"), "-v"},
                chains, 2, hopped_to, "so it is a library and there is no executable to write. "
                .. "Remove '--output' to compile it as a library")

            -- '--toolchain' asks for one by name, and ENS_TOOLCHAIN is its environment spelling
            local by_option = path.join(root, "by option.exe")
            run(host_exe, {"build", hello, "--output", by_option, "--toolchain", "9.9.2", "-v"},
                chains, 0, "'--toolchain 9.9.2'", hopped_to, ": Built '")
            run_program(by_option, 0, "Hello, world!")
            run(host_exe, {"build", hello, "--output", path.join(root, "by variable.exe"), "-v"},
                {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "9.9.2"}, 0, "ENS_TOOLCHAIN=9.9.2",
                hopped_to, ": Built '")

            -- a declared version newer than this toolchain and than every installed one is a
            -- problem, at every verbosity
            run(host_exe, {"build", pkg}, nothing_installed, 2,
                slashed(pkg) .. "' " .. declared_newer,
                ". No installed toolchain is Ens 9.9 or later: '" .. slashed(empty)
                    .. "' contains no toolchain. Install Ens 9.9 or later in a folder named after "
                    .. "its version, such as '"
                    .. slashed(path.join(empty, "9.9.0", "ens" .. exe_suffix))
                    .. "', or write '--toolchain local' to build with this toolchain anyway",
                "!: Built '")

            -- one hop is all there is, however the chain starts
            local _, chained = run(installed_exe, {"build", pkg, "--output",
                path.join(root, "once.exe"), "-v"}, chains, 0, ": Built '")
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
            -- arguments intact, and a name that is not installed is refused rather than ignored.
            run(host_exe, {"run", pkg, "-v"}, chains, 0, hopped_to, "built by a delegate")
            run(host_exe, {"test", pkg, "-v"}, chains, 0, hopped_to, "There are no tests in")
            run(host_exe, {"run", hello, "--toolchain", "9.9"}, nothing_installed, 2,
                "'--toolchain 9.9' names Ens 9.9, which is not installed: '", "' contains no "
                    .. "toolchain")

            -- a build root that declares nothing newer still goes to a newer installed toolchain
            run(host_exe, {"build", hello, "--output", path.join(root, "undeclared.exe"), "-v"},
                chains, 0, "Ens 9.9.2 is installed at '" .. hopped_to .. "', and it is newer than "
                    .. "this toolchain, Ens ", ", so it runs the command", ": Built '")

            -- every other command goes to a newer toolchain too. The one installed here is a
            -- program that prints what it was handed, since a copy of this one would print the
            -- same thing whichever of the two ran.
            local stubs = path.join(root, "stubs")
            local stub_exe = path.join(stubs, "9.9.5", "ens" .. exe_suffix)
            local stub_source = path.join(root, "stub.ens")
            io.writefile(stub_source, [[
import Environment from @std.environment;
import @std.environment;

main() -> int {
    string line = "stub ran:";
    for (let argument in environment.arguments()) {
        line = line + " " + argument;
    }
    print(line);
    print("ENS_TOOLCHAIN=" + (Environment.current().get("ENS_TOOLCHAIN") ?? ""));
    return 7;
}
]])
            os.mkdir(path.directory(stub_exe))
            run(host_exe, {"build", stub_source, "--output", stub_exe}, nothing_installed, 0)
            local stubbed = {ENS_TOOLCHAINS = stubs}
            local handed = "ENS_TOOLCHAIN=local"
            run(host_exe, {"version"}, stubbed, 7, "stub ran: version", handed)
            run(host_exe, {"--version"}, stubbed, 7, "stub ran: --version")
            run(host_exe, {"help", "build"}, stubbed, 7, "stub ran: help build")
            run(host_exe, {"override", "list"}, stubbed, 7, "stub ran: override list")
            run(host_exe, {"version", "--toolchain", "local"}, stubbed, 0, version_line, "!stub ran")
            run(host_exe, {"version"}, {ENS_TOOLCHAINS = stubs, ENS_TOOLCHAIN = "local"}, 0,
                version_line, "!stub ran")

            -- a command line this toolchain cannot read is handed over as it was written, unless it
            -- names a toolchain this one could not read out of it
            run(host_exe, {"frobnicate", "--widely", "a b"}, stubbed, 7,
                "stub ran: frobnicate --widely a b", handed)
            run(host_exe, {"frobnicate", "--toolchain", "9.9.5"}, stubbed, 2, "ens: ",
                "!stub ran")
            run(host_exe, {"frobnicate"}, nothing_installed, 2, "ens: ", "!stub ran")

            -- a toolchain the environment names is required by every command
            run(host_exe, {"version"}, {ENS_TOOLCHAINS = toolchains, ENS_TOOLCHAIN = "9.9"}, 2,
                "'ENS_TOOLCHAIN=9.9' names Ens 9.9, which is not installed: '",
                "or write '--toolchain local' to use this toolchain, Ens ")

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
        -- '-y' is what keeps the nested build from asking: a toolchain re-check offers to fetch the
        -- packages the language server's own toolchain names, and this process has no standard
        -- input to answer with, so a prompt would hang the whole run.
        local function run_lsp_build(job)
            local log = path.join(out_dir, job.name .. ".log")
            local rc = execMerged("xmake", {"build", "-y", "ens-lsp"}, log)
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
            if job.standalone then return run_standalone(job) end
            if job.linux_link then return run_linux_link(job) end
            if job.windows_link then return run_windows_link(job) end
            if job.symbol_audit then return run_symbol_audit(job) end
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
            local scope_problem     = nil   -- why a directive's '-at' cannot be read, if it cannot
            local fixture_level     = "O2"  -- the level this job builds every fixture at
            local expected_errors   = {}
            local expected_notes    = {}    -- @expect-note: note lines the diagnostics point with
            local ens_test_args     = job.ens_test_args   -- @ens-test: run `ens test` on the folder instead
            local stdlib_root       = nil   -- @stdlib: a std/ package to compile against instead of libs/
            local content = (ens_file and io.readfile(ens_file)) or ""
            if ens_file then
                local named, unknown = fixturePlatforms(ens_file)
                if #unknown > 0 then
                    return {name = name, ok = false, short = "@platform names no host",
                        full = string.format("%s: '@platform %s' names no host; write 'linux', "
                            .. "'macos' or 'windows'", name, unknown[1])}
                end
                if not runsHere(named) then
                    return {name = name, ok = true, note = "skipped for this platform"}
                end
            end
            for line in content:gmatch("[^\r\n]+") do
                local stdlib_str = line:match("^%s*//%s*@stdlib%s+(%S+)")
                if stdlib_str then
                    stdlib_root = path.absolute(stdlib_str, path.directory(ens_file))
                end
                local exit_str = line:match("^%s*//%s*@exit%s+(%-?%d+)")
                if exit_str then expected_exit = tonumber(exit_str) end
                local contains_str = line:match("^%s*//%s*@stdout%-contains%s+(.*)$")
                if contains_str then table.insert(expected_contains, contains_str) end
                local stdout_str = line:match("^%s*//%s*@stdout (.*)$")
                if stdout_str then
                    if expected_stdout == nil then expected_stdout = {} end
                    table.insert(expected_stdout, stdout_str)
                end
                local err_contains_str = line:match("^%s*//%s*@stderr%-contains%s+(.*)$")
                if err_contains_str then
                    table.insert(expected_stderr_contains, err_contains_str)
                end
                local stderr_str = line:match("^%s*//%s*@stderr (.*)$")
                if stderr_str then
                    if expected_stderr == nil then expected_stderr = {} end
                    table.insert(expected_stderr, stderr_str)
                end
                -- a pin scoped to a level counts here only when it names the level this job
                -- builds at; '@expect-error-at' names a phase and is semacheck's
                local kind, scope = line:match("^%s*//%s*@([%a%-]-)%-at(.*)$")
                local level, scoped = nil, nil
                if scope then
                    level, scoped = scope:match("^ (%S+) (.*)$")
                end
                if kind == nil or kind == "expect-error" then
                    -- not a scoped directive
                elseif kind ~= "stdout" and kind ~= "stderr" and kind ~= "stdout-contains"
                        and kind ~= "stderr-contains" then
                    scope_problem = scope_problem or {short = "'-at' on a directive that takes "
                        .. "no level", full = string.format("'@%s' cannot be scoped to a "
                        .. "level. Only '@stdout', '@stderr', '@stdout-contains' and "
                        .. "'@stderr-contains' take '-at'", kind)}
                elseif level == nil then
                    scope_problem = scope_problem or {short = "a scoped pin has no line",
                        full = string.format("'@%s-at' needs a level and then the line it pins, "
                            .. "as in '// @%s-at O2 <line>'", kind, kind)}
                elseif level ~= "O0" and level ~= "O2" then
                    scope_problem = scope_problem or {
                        short = "a pin is scoped to a level no fixture is built at",
                        full = string.format("a pin is scoped to '%s', and fixtures are built "
                            .. "only at 'O0' and 'O2'. Write '-at O0' or '-at O2' instead", level)}
                else
                    if level == fixture_level then
                        if kind == "stdout" then
                            expected_stdout = expected_stdout or {}
                            table.insert(expected_stdout, scoped)
                        elseif kind == "stderr" then
                            expected_stderr = expected_stderr or {}
                            table.insert(expected_stderr, scoped)
                        elseif kind == "stdout-contains" then
                            table.insert(expected_contains, scoped)
                        else
                            table.insert(expected_stderr_contains, scoped)
                        end
                    end
                end
                local error_str = line:match("^%s*//%s*@expect%-error%s+(.*)$")
                if error_str then
                    table.insert(expected_errors, (error_str:gsub("{language_version}",
                        language_version)))
                end
                local note_str = line:match("^%s*//%s*@expect%-note%s+(.*)$")
                if note_str then
                    table.insert(expected_notes, (note_str:gsub("{language_version}",
                        language_version)))
                end
                local enstest_str = line:match("^%s*//%s*@ens%-test%s*(.*)$")
                if enstest_str then
                    ens_test_args = {}
                    for token in enstest_str:gmatch("%S+") do
                        table.insert(ens_test_args, token)
                    end
                end
            end

            if scope_problem then
                return {name = name, ok = false, short = scope_problem.short,
                    full = string.format("%s: %s", name, scope_problem.full)}
            end

            -- a note is one diagnostic's second location, so it is only ever asserted beside the
            -- diagnostic carrying it; a fixture pinning a note alone says nothing about which
            -- error that note belongs to.
            if #expected_notes > 0 and #expected_errors == 0 then
                return {name = name, ok = false, short = "@expect-note without @expect-error",
                    full = string.format("%s: @expect-note needs the @expect-error whose note it "
                        .. "is; add that line or drop the note", name)}
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
            -- builds link through ens-llvm and some of the suites bind native libraries, so the job
            -- carries the same linker environment the codegen harness does.
            if ens_test_args ~= nil then
                os.tryrm(stdout_file)
                os.tryrm(stderr_file)
                local argv = {"test", job.source}
                if stdlib_root then
                    table.insert(argv, "--stdlib")
                    table.insert(argv, stdlib_root)
                end
                table.join2(argv, treeTargetArguments())
                for _, a in ipairs(ens_test_args) do
                    table.insert(argv, (a:gsub("{dir}", (job.source:gsub("\\", "/")))))
                end
                local env = linkEnvironment()
                -- a package's own tests run in an empty folder of their own, which has to stay
                -- empty: whatever a test writes goes in a scratch folder it removes, never in the
                -- folder it ran in
                local run_options = {envs = env}
                local run_folder = nil
                if not ens_file then
                    run_folder = path.join(out_dir, name .. ".cwd")
                    os.tryrm(run_folder)
                    os.mkdir(run_folder)
                    run_options.curdir = run_folder
                end
                local run_rc = execSplit(host_exe, argv, stdout_file, stderr_file, run_options)
                local actual_stdout = captured(stdout_file)
                local actual_stderr = captured(stderr_file)
                local why = compareRun(run_rc, actual_stdout, actual_stderr)
                if run_folder then
                    local left = os.filedirs(path.join(run_folder, "*"))
                    if #left > 0 then
                        table.insert(why, string.format("the tests left %s in the folder they "
                            .. "ran in", path.filename(left[1])))
                    end
                    os.tryrm(run_folder)
                end
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
            local env = linkEnvironment()
            local build_argv = {"build", job.source, "--output", exe_file, "--objects", out_dir}
            if stdlib_root then
                table.insert(build_argv, "--stdlib")
                table.insert(build_argv, stdlib_root)
            end
            local compile_rc = execMerged(host_exe, build_argv, compile_log, {envs = env})
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
                for _, expected in ipairs(expected_notes) do
                    if not compile_log_text:find(expected, 1, true) then
                        table.insert(why, string.format("note %q not found in stderr", expected))
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
        -- one of them. The bridge is first. The compiler is the first program linked through it.
        print("Building the ens-llvm native archive...")
        buildNativeArchive()
        locateSeed()
        print(string.format("Building the compiler with the pinned %s seed...", seed_host))
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
