-- compile each tests/*.ens with the ens compiler and verify the exit code (and stdout)
-- following the test source header:
--     // @exit 12
--     // @stdout Hello!
-- use @expect-error instead to assert the compiler reports a specific diagnostic.
--     // @expect-error Undefined function 'testFunction'
task("test")
    set_menu({
        usage = "xmake test",
        description = "Run all Ens tests in the tests/ folder",
        options = {}
    })
    on_run(function()
        import("core.project.config")
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

        local total, passed = 0, 0
        local failures = {}

        -- list of tests:
        --   * every tests/*.ens file (single-file mode)
        --   * every tests/<dir>/main.ens (folder mode)
        local jobs = {}
        for _, ens_file in ipairs(os.files(path.join(tests_dir, "*.ens"))) do
            table.insert(jobs, {
                name = path.basename(ens_file),
                ens_file = ens_file,
                source = ens_file,
            })
        end
        for _, sub in ipairs(os.dirs(path.join(tests_dir, "*"))) do
            local main_ens = path.join(sub, "main.ens")
            if os.isfile(main_ens) then
                table.insert(jobs, {
                    name = path.basename(sub),
                    ens_file = main_ens,
                    source = sub,
                })
            end
        end

        for _, job in ipairs(jobs) do
            total = total + 1
            local name = job.name
            local ens_file = job.ens_file
            local exe_file    = path.join(out_dir, name .. ".exe")
            local stdout_file = path.join(out_dir, name .. ".stdout")
            local compile_log = path.join(out_dir, name .. ".compile.log")

            local expected_exit   = 0
            local expected_stdout = nil
            local expected_error  = nil
            local content = io.readfile(ens_file) or ""
            for line in content:gmatch("[^\r\n]+") do
                local exit_str = line:match("^%s*//%s*@exit%s+(%-?%d+)")
                if exit_str then expected_exit = tonumber(exit_str) end
                local stdout_str = line:match("^%s*//%s*@stdout%s+(.*)$")
                if stdout_str then expected_stdout = stdout_str end
                local error_str = line:match("^%s*//%s*@expect%-error%s+(.*)$")
                if error_str then expected_error = error_str end
            end

            os.tryrm(exe_file)
            os.tryrm(stdout_file)

            local compile_rc = os.execv(ens_exe,
                {"--source", job.source, "--output", exe_file},
                {try = true, stdout = compile_log, stderr = compile_log})
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
                    passed = passed + 1
                    print(string.format("\27[32mPASS\27[0m %s", name))
                else
                    table.insert(failures, string.format("%s: %s\n%s", name,
                        table.concat(why, "; "),
                        compile_log_text:gsub("[\r\n]+$", "")))
                    print(string.format("\27[31mFAIL\27[0m %s - %s", name, table.concat(why, "; ")))
                end
                goto continue
            end

            if not os.isfile(exe_file) then
                table.insert(failures, string.format("%s: compile failed (exit %s)\n%s",
                    name, tostring(compile_rc),
                    compile_log_text:gsub("[\r\n]+$", "")))
                print(string.format("\27[31mFAIL\27[0m %s - compile failed", name))
                goto continue
            end

            local run_rc = os.execv(exe_file, {},
                {try = true, stdout = stdout_file, stderr = stdout_file})
            local actual_stdout = (io.readfile(stdout_file) or ""):gsub("[\r\n]+$", "")

            local ok = true
            local why = {}
            if run_rc ~= expected_exit then
                ok = false
                table.insert(why, string.format("exit=%s expected=%s",
                    tostring(run_rc), tostring(expected_exit)))
            end
            if expected_stdout ~= nil and actual_stdout ~= expected_stdout then
                ok = false
                table.insert(why, string.format("stdout=%q expected=%q",
                    actual_stdout, expected_stdout))
            end

            if ok then
                passed = passed + 1
                print(string.format("\27[32mPASS\27[0m %s", name))
            else
                table.insert(failures, string.format("%s: %s", name, table.concat(why, "; ")))
                print(string.format("\27[31mFAIL\27[0m %s - %s", name, table.concat(why, "; ")))
            end
            ::continue::
        end

        print(string.format("\n%d/%d tests passed", passed, total))
        if passed < total then
            for _, f in ipairs(failures) do print("  - " .. f) end
            os.raise("%d test failure(s)", total - passed)
        end
    end)
