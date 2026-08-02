-- Overrides the xmake-repo definition of lsp-framework. Upstream's CMake runs the code generator
-- by bare name (`COMMAND lspgen ...`) and relies on CMake substituting the target's path, which
-- does not happen everywhere; the CI runners then fail with "lspgen: not found". Naming the built
-- tool explicitly resolves it on every generator. Delete this file when upstream spells it
-- $<TARGET_FILE:lspgen> itself.
package("lsp-framework")
    set_homepage("https://github.com/leon-bckl/lsp-framework")
    set_description("Language Server Protocol implementation in C++")
    set_license("MIT")

    add_urls("https://github.com/leon-bckl/lsp-framework/archive/refs/tags/$(version).tar.gz",
             "https://github.com/leon-bckl/lsp-framework.git")
    add_versions("1.3.1", "f5c53e85c407d3773c7b917f4cab777da12d4059a5aeb2dbc3ec656fa7842569")

    add_deps("cmake")

    if is_plat("linux", "bsd") then
        add_syslinks("pthread")
    elseif is_plat("windows", "mingw") then
        add_syslinks("ws2_32")
    end

    on_install("windows", "linux", "macosx", "mingw@windows", "bsd", function (package)
        io.replace("CMakeLists.txt", "COMMAND%s+lspgen%s+", "COMMAND $<TARGET_FILE:lspgen> ",
            {plain = false})
        io.replace("CMakeLists.txt", "install(TARGETS lsp EXPORT lsp ARCHIVE LIBRARY)",
            "install(TARGETS lsp EXPORT lsp RUNTIME ARCHIVE LIBRARY)", {plain = true})
        local configs = {"-DCMAKE_INSTALL_LIBDIR=lib"}
        if package:is_plat("windows") and package:config("shared") then
            table.insert(configs, "-DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON")
        end
        table.insert(configs, "-DLSP_USE_SANITIZERS=" .. (package:config("asan") and "ON" or "OFF"))
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <lsp/messages.h>
            #include <lsp/connection.h>
            #include <lsp/io/standardio.h>
            #include <lsp/messagehandler.h>
            void test() {
                auto connection     = lsp::Connection(lsp::io::standardIO());
                auto messageHandler = lsp::MessageHandler(connection);
            }
        ]]}, {configs = {languages = "c++20"}}))
    end)
package_end()
