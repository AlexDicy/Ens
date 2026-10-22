-- The C++ runtime `ens` carries on macOS, libc++ and libc++abi as static archives built here from
-- LLVM's sources, so that it is built for the oldest macOS `ens` runs on like everything else in
-- ens-llvm. The LLVM toolchain package builds its own copy, but for the machine it was built on,
-- and LLVM's build passes no deployment target down to its runtimes.
--
-- The archives are hermetic: nothing in them is visible outside the program that links them. A
-- program that exported them would offer dyld its copy of the runtime's weak definitions, which
-- dyld coalesces with the copy in the system's libraries, in one direction or the other depending
-- on the program's fixup format, and a program then runs a libc++ it was not built with.
package("libcxx")
    set_kind("library")
    set_homepage("https://libcxx.llvm.org/")
    set_description("libc++ and libc++abi as static archives, built from LLVM's sources.")

    add_urls("https://github.com/llvm/llvm-project/releases/download/llvmorg-$(version)/llvm-project-$(version).src.tar.xz")
    add_versions("21.1.0", "1672e3efb4c2affd62dbbe12ea898b28a451416c7d95c1bd0190c26cbe878825")

    add_deps("cmake", "ninja")
    add_links("c++", "c++abi")

    on_install("macosx", function (package)
        os.cd("runtimes")
        local configs = {
            "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DLIBCXX_ENABLE_SHARED=OFF",
            "-DLIBCXXABI_ENABLE_SHARED=OFF",
            "-DLIBCXX_HERMETIC_STATIC_LIBRARY=ON",
            "-DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON",
            "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
            "-DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=OFF",
            "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
            "-DLIBCXX_INCLUDE_TESTS=OFF",
            "-DLIBCXXABI_INCLUDE_TESTS=OFF",
            "-DLIBCXX_INSTALL_MODULES=OFF",
        }
        import("package.tools.cmake").install(package, configs, {cmake_generator = "Ninja"})
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("lib"), "libc++.a")))
        assert(os.isfile(path.join(package:installdir("lib"), "libc++abi.a")))
    end)
