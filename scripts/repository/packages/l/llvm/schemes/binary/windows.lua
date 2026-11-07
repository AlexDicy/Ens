-- See linux.lua.
function add_urls(scheme, arch)
    if arch == "x64" then
        scheme:add("urls", "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-windows-x64-llvm.tar.xz", {alias = "archive"})
        scheme:add("versions", "archive:21.1.0", "191b345596f426126f670ba87dd971a7ded982e749aa15dbf7496f704edbca76")
    elseif arch == "arm64" then
        scheme:add("urls", "https://github.com/AlexDicy/llvm-builds/releases/download/llvm-21.1.0-r2/llvm-21.1.0-windows-arm64-llvm.tar.xz", {alias = "archive"})
        scheme:add("versions", "archive:21.1.0", "d2c0437e58304f40fa67c1d17ce556c524aa68503e4c3074b6f234e2ebc5c8b7")
    end
end
