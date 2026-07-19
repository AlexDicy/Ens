-- compile each tests/*.ens with the ens compiler and verify the exit code (and stdout)
-- following the test source header:
--     // @exit 12
--     // @stdout Hello!
-- use @expect-error instead to assert the compiler reports a specific diagnostic.
--     // @expect-error Undefined function 'testFunction'
-- a folder test's main.ens may use @ens-test (optionally with extra arguments) to run
-- `ens test --source <folder> ...` instead of compile+run, asserting on its output.
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
            local name = path.basename(sub)
            if os.isfile(main_ens) and want(name) then
                table.insert(jobs, {
                    name = name,
                    ens_file = main_ens,
                    source = sub,
                })
            end
        end

        -- the self-hosted front end's own tests run as one `ens test` job.
        if want("selfhost_frontend") then
            table.insert(jobs, {
                name = "selfhost_frontend",
                source = path.join(os.projectdir(), "selfhost", "frontend", "src"),
                ens_test_args = {"--tests", path.join(os.projectdir(), "selfhost", "frontend", "tests")},
            })
        end

        -- the syntax.grammar to Ens code generator's tests run the same way.
        if want("selfhost_syntaxgen") then
            table.insert(jobs, {
                name = "selfhost_syntaxgen",
                source = path.join(os.projectdir(), "selfhost", "syntaxgen", "src"),
                ens_test_args = {"--tests", path.join(os.projectdir(), "selfhost", "syntaxgen", "tests")},
            })
        end

        -- the self-hosted semantic layer's tests run the same way.
        if want("selfhost_sema") then
            table.insert(jobs, {
                name = "selfhost_sema",
                source = path.join(os.projectdir(), "selfhost", "sema", "src"),
                ens_test_args = {"--tests", path.join(os.projectdir(), "selfhost", "sema", "tests")},
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
        local function execMerged(program, argv, logpath)
            local logfile = io.open(logpath, "w")
            local rc = os.execv(program, argv, {try = true, stdout = logfile, stderr = logfile})
            logfile:close()
            return rc
        end

        -- the corpus round-trip harness: build the driver exe fresh from its own workspace (it
        -- imports the front end as the @ens.frontend package), enumerate every .ens file in the
        -- real source trees into a manifest, and run the driver over it.
        local function run_corpus(job)
            local name = job.name
            local corpus_dir  = path.join(os.projectdir(), "build", "corpus")
            local exe_file    = path.join(corpus_dir, "corpus.exe")
            local manifest    = path.join(corpus_dir, "manifest.txt")
            local log         = path.join(out_dir, name .. ".log")
            local corpus_src   = path.join(os.projectdir(), "selfhost", "corpus", "src")

            if not os.isdir(corpus_dir) then
                os.mkdir(corpus_dir)
            end
            os.tryrm(exe_file)
            local compile_rc = execMerged(ens_exe, {"--source", corpus_src, "--output", exe_file}, log)
            if not os.isfile(exe_file) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s",
                        name, tostring(compile_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end

            -- enumerate the corpus.
            local files = {}
            for _, root in ipairs({"selfhost", "libs", "tests"}) do
                for _, f in ipairs(os.files(path.join(os.projectdir(), root, "**.ens"))) do
                    table.insert(files, (f:gsub("\\", "/")))
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
            local check_src = path.join(os.projectdir(), "selfhost", "semacheck", "src")

            if not os.isdir(check_dir) then
                os.mkdir(check_dir)
            end
            os.tryrm(exe_file)
            local compile_rc = execMerged(ens_exe, {"--source", check_src, "--output", exe_file}, log)
            if not os.isfile(exe_file) then
                return {name = name, ok = false, short = "harness build failed",
                    full = string.format("%s: harness build failed (exit %s)\n%s",
                        name, tostring(compile_rc), (io.readfile(log) or ""):gsub("[\r\n]+$", ""))}
            end

            -- enumerate the program units.
            local function slashed(p) return (p:gsub("\\", "/")) end
            local lines = {"stdlib " .. slashed(path.join(os.projectdir(), "libs"))}
            local function add_unit(label, source, seeds)
                table.insert(lines, "unit " .. label)
                table.insert(lines, "source " .. slashed(source))
                for _, seed in ipairs(seeds) do
                    table.insert(lines, "seed " .. seed)
                end
            end
            local singles = os.files(path.join(tests_dir, "*.ens"))
            table.sort(singles)
            for _, f in ipairs(singles) do
                add_unit("tests/" .. path.filename(f), tests_dir, {path.filename(f)})
            end
            local folders = os.dirs(path.join(tests_dir, "*"))
            table.sort(folders)
            for _, sub in ipairs(folders) do
                if os.isfile(path.join(sub, "main.ens")) then
                    add_unit("tests/" .. path.basename(sub), sub, {"main.ens"})
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

        -- run a single test: compile, optionally run, and compare against the header.
        -- returns { name = ..., ok = bool, short = <fail reason>, full = <detailed report> }.
        local function run_one(job)
            if job.corpus then return run_corpus(job) end
            if job.semacheck then return run_semacheck(job) end
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
                local argv = {"test", "--source", job.source}
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
                {"--source", job.source, "--output", exe_file}, compile_log)
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
