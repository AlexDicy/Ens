-- See linux.lua. macOS is arm64 only here, as the deployment target this tree builds for is.
function add_urls(scheme, arch)
    if arch == "arm64" then
        scheme:add("urls", "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-macos-arm64-llvm.tar.xz", {alias = "archive"})
        scheme:add("versions", "archive:21.1.0", "14bdfaec139cc73e970eade4de0d59ea586d47fa38b79df9973eb3d6ae6d4000")
    end
end
