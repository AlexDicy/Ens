-- The toolchain comes from AlexDicy/llvm-builds, which builds LLVM with the configuration this
-- tree asks for and publishes it per platform, so the compiler and the libraries ens links come
-- from one build on every platform.
function add_urls(scheme, arch)
    if arch == "x86_64" then
        scheme:add("urls", "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-linux-x64-llvm.tar.xz", {alias = "archive"})
        scheme:add("versions", "archive:21.1.0", "b854d5f70450d36b3b19f05813b4ecd76b6038415853f49e78d4a4d9bc36a9c6")
    elseif arch == "arm64" then
        scheme:add("urls", "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-linux-arm64-llvm.tar.xz", {alias = "archive"})
        scheme:add("versions", "archive:21.1.0", "3ad6dd3759f088171de3f678ce072e142412f5bd15c00580b9298cfc9ac8628b")
    end
end
