-- The toolchain comes from AlexDicy/llvm-builds, which builds LLVM with the configuration this
-- tree asks for and publishes it per platform, so the compiler and the libraries ens links come
-- from one build everywhere. xmake cannot change an external package's download, so this replaces
-- xmake-repo's llvm package with one that only downloads: no source build, no components.
package("llvm")
    set_kind("toolchain")
    set_homepage("https://llvm.org/")
    set_description("The LLVM Compiler Infrastructure")

    local release = "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-"
    local linux_release = "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r3/llvm-21.1.0-"
    if is_plat("linux") then
        if is_arch("arm64", "aarch64") then
            add_urls(linux_release .. "linux-arm64-llvm.tar.xz")
            add_versions("21.1.0", "e0bebd397cc75b036553d9b7aeba336348c7063ceefb0557457fe150285088f4")
        else
            add_urls(linux_release .. "linux-x64-llvm.tar.xz")
            add_versions("21.1.0", "32e4c2a65a4450863d50a4917f848c16d4b0e8bb0e68962dbcb52ec488986a1a")
        end
    elseif is_plat("macosx") then
        add_urls(release .. "macos-arm64-llvm.tar.xz")
        add_versions("21.1.0", "14bdfaec139cc73e970eade4de0d59ea586d47fa38b79df9973eb3d6ae6d4000")
    elseif is_plat("windows") then
        if is_arch("arm64") then
            add_urls(release .. "windows-arm64-llvm.tar.xz")
            add_versions("21.1.0", "d2c0437e58304f40fa67c1d17ce556c524aa68503e4c3074b6f234e2ebc5c8b7")
        else
            add_urls(release .. "windows-x64-llvm.tar.xz")
            add_versions("21.1.0", "191b345596f426126f670ba87dd971a7ded982e749aa15dbf7496f704edbca76")
        end
    end

    on_install(function (package)
        os.cp("*", package:installdir(), {symlink = true})
    end)

    on_test(function (package)
        if not package:is_plat("windows") then
            os.vrun("llvm-config --version")
        end
        os.vrun("clang --version")
    end)
