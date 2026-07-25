-- compile each tests/*.ens with the ens compiler and verify the exit code (and stdout)
-- following the test source header:
--     // @exit 12
--     // @stdout Hello!
-- use @expect-error instead to assert the compiler reports a specific diagnostic.
--     // @expect-error Undefined function 'testFunction'
-- a folder test's main.ens may use @ens-test (optionally with extra arguments) to run
-- `ens test <folder> ...` instead of compile+run, asserting on its output.
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
            for _, pkg in ipairs({"corpus", "frontend", "sema", "semacheck", "syntaxgen"}) do
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

        -- run a single test: compile, optionally run, and compare against the header.
        -- returns { name = ..., ok = bool, short = <fail reason>, full = <detailed report> }.
        local function run_one(job)
            if job.corpus then return run_corpus(job) end
            if job.semacheck then return run_semacheck(job) end
            if job.cli_core then return run_cli_core(job) end
            if job.cli_workspace then return run_cli_workspace(job) end
            local name = job.name
            local ens_file = job.ens_file
            local exe_file    = path.join(out_dir, name .. ".exe")
            local stdout_file = path.join(out_dir, name .. ".stdout")
            local compile_log = path.join(out_dir, name .. ".compile.log")

            local expected_exit     = 0
            local expected_stdout   = nil   -- list of lines; joined with "\n" for an exact match
            local expected_contains = {}    -- substrings that must each appear in stdout
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

            -- compare a process result against the @exit/@stdout/@stdout-contains directives.
            local function compareRun(run_rc, actual_stdout)
                local why = {}
                if run_rc ~= expected_exit then
                    table.insert(why, string.format("exit=%s expected=%s",
                        tostring(run_rc), tostring(expected_exit)))
                end
                if expected_stdout ~= nil then
                    local joined = table.concat(expected_stdout, "\n")
                    if actual_stdout ~= joined then
                        table.insert(why, string.format("stdout=%q expected=%q",
                            actual_stdout, joined))
                    end
                end
                for _, sub in ipairs(expected_contains) do
                    if not actual_stdout:find(sub, 1, true) then
                        table.insert(why, string.format("stdout missing %q", sub))
                    end
                end
                return why
            end

            -- @ens-test: invoke `ens test` on the folder and assert on its combined output.
            if ens_test_args ~= nil then
                os.tryrm(stdout_file)
                local argv = {"test", job.source}
                for _, a in ipairs(ens_test_args) do
                    table.insert(argv, (a:gsub("{dir}", (job.source:gsub("\\", "/")))))
                end
                local run_rc = execMerged(ens_exe, argv, stdout_file)
                local actual_stdout = (io.readfile(stdout_file) or ""):gsub("[\r\n]+$", "")
                local why = compareRun(run_rc, actual_stdout)
                if #why == 0 then
                    return {name = name, ok = true}
                end
                local short = table.concat(why, "; ")
                return {name = name, ok = false, short = short,
                    full = string.format("%s: %s\n%s", name, short, actual_stdout)}
            end

            os.tryrm(exe_file)
            os.tryrm(stdout_file)

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

            local run_rc = execMerged(exe_file, {}, stdout_file)
            local actual_stdout = (io.readfile(stdout_file) or ""):gsub("[\r\n]+$", "")

            local why = compareRun(run_rc, actual_stdout)

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
