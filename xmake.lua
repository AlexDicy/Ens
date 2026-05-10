add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

add_requires("llvm")
add_requires("libllvm", { configs = { lld = true } })
add_requires("libxml2", { configs = { runtimes = "MT" } })

if is_plat("windows") then
    -- libllvm is MT only
    set_runtimes("MT")
end


target("ens-frontend")
    set_kind("static")
    set_languages("cxx17")
    add_files("compiler/frontend/**.cpp")
    add_headerfiles("compiler/frontend/**.h")
    add_includedirs("compiler/frontend", { public = true })


target("ens-codegen")
    set_kind("static")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_files("compiler/codegen/**.cpp")
    add_headerfiles("compiler/codegen/**.h")
    add_includedirs("compiler/codegen", { public = true })
    add_deps("ens-frontend")
    add_packages("libllvm", "libxml2", { public = true })
    add_links("LLVMCGData")


target("ens")
    set_kind("binary")
    set_languages("cxx17")
    set_toolchains("@llvm")
    add_files("compiler/driver/**.cpp")
    add_headerfiles("compiler/driver/**.h")
    add_deps("ens-frontend", "ens-codegen")


target("ens-lsp")
    set_kind("binary")
    set_languages("cxx17")
    add_files("lsp/**.cpp")
    add_headerfiles("lsp/**.h")
    add_deps("ens-frontend")


-- compile each tests/*.ens with the ens compiler and verify
-- the exit code (and stdout) following the test source header:
--     // @exit 12
--     // @stdout Hello!
task("test")
    set_menu({
        usage = "xmake test",
        description = "Run all Ens tests in the tests/ folder",
        options = {}
    })
    on_run(function()
        local mode = is_mode("release") and "release" or "debug"
        local ens_exe = path.join(os.projectdir(), "build", os.host(), os.arch(), mode, "ens.exe")
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

        for _, ens_file in ipairs(os.files(path.join(tests_dir, "*.ens"))) do
            total = total + 1
            local name = path.basename(ens_file)
            local exe_file    = path.join(out_dir, name .. ".exe")
            local stdout_file = path.join(out_dir, name .. ".stdout")
            local compile_log = path.join(out_dir, name .. ".compile.log")

            local expected_exit   = 0
            local expected_stdout = nil
            local content = io.readfile(ens_file) or ""
            for line in content:gmatch("[^\r\n]+") do
                local exit_str = line:match("^%s*//%s*@exit%s+(%-?%d+)")
                if exit_str then expected_exit = tonumber(exit_str) end
                local stdout_str = line:match("^%s*//%s*@stdout%s+(.*)$")
                if stdout_str then expected_stdout = stdout_str end
            end

            os.tryrm(exe_file)
            os.tryrm(stdout_file)

            local compile_rc = os.execv(ens_exe,
                {"--source", ens_file, "--output", exe_file},
                {try = true, stdout = compile_log, stderr = compile_log})
            if not os.isfile(exe_file) then
                table.insert(failures, string.format("%s: compile failed (exit %s)\n%s",
                    name, tostring(compile_rc),
                    (io.readfile(compile_log) or ""):gsub("[\r\n]+$", "")))
                print(string.format("\27[31mFAIL\27[0m %s — compile failed", name))
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
                print(string.format("\27[31mFAIL\27[0m %s — %s", name, table.concat(why, "; ")))
            end
            ::continue::
        end

        print(string.format("\n%d/%d tests passed", passed, total))
        if passed < total then
            for _, f in ipairs(failures) do print("  - " .. f) end
            os.raise("%d test failure(s)", total - passed)
        end
    end)
