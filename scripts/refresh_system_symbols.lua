-- Rewrites the symbol lists ens links a program's system libraries against, from the libraries of
-- the machine it runs on: `xmake l scripts/refresh_system_symbols.lua`. Only this machine's own lists
-- are rewritten, and what was added and removed is printed. Input it cannot read stops it before
-- anything is written, so a list never comes out shorter than what the platform provides.
--
-- A Linux list can also be read from a folder holding another machine's libraries, such as the
-- unpacked libc6 and libgcc-s1 packages of a distribution:
-- `xmake l scripts/refresh_system_symbols.lua linux-arm64 <folder>`.
-- On Windows both Windows lists are read from the Windows SDK, which holds the libraries of every
-- processor, and from another machine they can be read from that SDK's library folder:
-- `xmake l scripts/refresh_system_symbols.lua windows "<Windows Kits>/10/Lib/<version>"`.
--
-- A list is text of our own, one entry per line, read by selfhost/bundled/src/symbols.ens:
--   sdk <version>                    the version of the SDK a macOS or Windows list was read from
--   glibc <version>                  the oldest glibc a program linked against a Linux list runs on
--   library <name> [<version>]       the library the entries after it belong to: its install name
--                                    and current version on macOS, its soname on Linux, its DLL
--                                    on Windows
--   symbol <name> [<version>]        code the library exports, with its version node on Linux
--   weak-symbol <name>               code a macOS library exports as a weak definition
--   object <name> <version> <size>   data a Linux library exports, with its size in bytes
--   tls <name> <version> <size>      thread-local data a Linux library exports
--   data <name>                      data a Windows DLL exports, which a program reads through
--                                    the pointer its import table holds
-- Lines starting with '#' are comments.

import("core.base.json")
import("core.base.global")
import("lib.detect.find_tool")

local lists = path.join(os.scriptdir(), "..", "selfhost", "bundled", "symbols")

-- The quoted or plain names of a tbd list, written between '[' and ']' and possibly across lines.
local function names(text)
    local found = {}
    for item in text:gmatch("[^,]+") do
        local name = item:match("^%s*'(.-)'%s*$") or item:match("^%s*(%S+)%s*$")
        if name then
            table.insert(found, name)
        end
    end
    return found
end

-- Each section a tbd document lists under `key:`, as {targets = {...}, lists = {kind = names}}.
local function sections(document, key)
    local block = document:match("\n" .. key .. ":%s*\n(.-)\n%S") or document:match("\n" .. key
        .. ":%s*\n(.*)$")
    local found = {}
    if not block then
        return found
    end
    local current = nil
    for field, body in block:gmatch("([%a%-]+):%s*(%b[])") do
        local listed = names(body:sub(2, -2))
        if field == "targets" then
            current = {targets = listed, lists = {}}
            table.insert(found, current)
        elseif current then
            current.lists[field] = listed
        else
            raise("the '%s' block of a tbd document lists '%s' before any targets", key, field)
        end
    end
    return found
end

local function contains(list, value)
    for _, item in ipairs(list) do
        if item == value then
            return true
        end
    end
    return false
end

-- The documents of a tbd file: each one starts at a '--- !tapi-tbd' line, and the file ends with
-- a '...' line.
local function tbdDocuments(text)
    local found = {}
    local current = nil
    for line in text:gmatch("([^\n]*)\n?") do
        if line == "--- !tapi-tbd" then
            current = {}
            table.insert(found, current)
        elseif line ~= "..." and current then
            table.insert(current, line)
        end
    end
    for i, lines in ipairs(found) do
        found[i] = "\n" .. table.concat(lines, "\n") .. "\n"
    end
    return found
end

local function servesArm64(targets)
    return contains(targets, "arm64-macos") or contains(targets, "arm64e-macos")
end

-- Whether `name` is a '$ld$' directive that changes a symbol only for programs targeting a macOS
-- before 11, which no arm64 Mac runs. Any other directive stops the script, so a new kind is looked
-- at before it reaches a link.
local function olderMacosDirective(name)
    if not name:startswith("$ld$") then
        return false
    end
    local major = name:match("^%$ld%$%a+%$os(%d+)%.")
    if major and tonumber(major) < 11 then
        return true
    end
    raise("libSystem lists the linker directive '%s', which this script does not know how to "
        .. "read", name)
end

-- What libSystem provides an arm64 macOS program, read from the SDK xcrun selects: its own exports
-- and those of every library it re-exports, which the system resolves through libSystem itself.
local function macosList()
    local sdk = os.iorunv("xcrun", {"--show-sdk-path"}):trim()
    local settings = json.loadfile(path.join(sdk, "SDKSettings.json"))
    local version = settings and settings.Version
    if not version then
        raise("'%s' has no 'Version' in its SDKSettings.json, so the list would not say which SDK "
            .. "it was read from", sdk)
    end
    local tbd = path.join(sdk, "usr", "lib", "libSystem.B.tbd")
    local text = io.readfile(tbd)
    if not text then
        raise("could not read '%s'", tbd)
    end
    local documents = {}
    for _, document in ipairs(tbdDocuments(text)) do
        local tbdVersion = document:match("\ntbd%-version:%s*(%d+)")
        if tbdVersion ~= "4" then
            raise("'%s' holds a document of tbd version %s; this script reads version 4 only",
                tbd, tostring(tbdVersion))
        end
        local installName = document:match("\ninstall%-name:%s*'([^']+)'")
        if not installName then
            raise("a document in '%s' has no install name", tbd)
        end
        documents[installName] = document
    end
    local systemName = "/usr/lib/libSystem.B.dylib"
    local system = documents[systemName]
    if not system then
        raise("'%s' does not describe '%s'", tbd, systemName)
    end
    local currentVersion = system:match("\ncurrent%-version:%s*([%d%.]+)")
    local included = {systemName}
    for _, section in ipairs(sections(system, "reexported%-libraries")) do
        if servesArm64(section.targets) then
            for _, library in ipairs(section.lists.libraries or {}) do
                if not documents[library] then
                    raise("libSystem re-exports '%s', which '%s' does not describe", library, tbd)
                end
                table.insert(included, library)
            end
        end
    end
    local kinds = {symbols = "symbol", ["weak-symbols"] = "weak-symbol"}
    local entries = {}
    for _, library in ipairs(included) do
        for _, key in ipairs({"exports", "reexports"}) do
            for _, section in ipairs(sections(documents[library], key)) do
                if servesArm64(section.targets) then
                    for field, listed in pairs(section.lists) do
                        local kind = kinds[field]
                        if kind then
                            for _, name in ipairs(listed) do
                                if not olderMacosDirective(name) then
                                    entries[kind .. " " .. name] = true
                                end
                            end
                        elseif not field:startswith("objc-") then
                            raise("'%s' lists '%s' for '%s', which this script does not read",
                                tbd, field, library)
                        end
                    end
                end
            end
        end
    end
    local lines = {}
    for entry in pairs(entries) do
        table.insert(lines, entry)
    end
    table.sort(lines)
    if #lines < 1000 then
        raise("'%s' gave only %d symbols for arm64, which is not the whole of libSystem", tbd,
            #lines)
    end
    local header = {
        "# What libSystem exports to an arm64 macOS program, read from the macOS " .. version
            .. " SDK.",
        "# Generated by scripts/refresh_system_symbols.lua; run it on a Mac instead of editing.",
        "sdk " .. version,
        "library " .. systemName .. " " .. currentVersion,
    }
    return "macos-arm64.symbols", table.concat(header, "\n") .. "\n" .. table.concat(lines, "\n")
        .. "\n"
end

-- The glibc a program linked against a Linux list needs at least, and the GCC whose libgcc_s it
-- needs: glibc 2.34 is the oldest a released ens runs on, and the oldest distributions that ship it
-- ship GCC 11. A version node newer than these is left out of a list, and a symbol's newest version
-- at or below them becomes its default. A symbol the libraries read keep only for programs linked
-- before, with no default version, is left out too, so reading a newer glibc can only drop a symbol
-- glibc itself stopped offering.
local glibcBaseline = {2, 34}
local gccBaseline = {11}

-- The libraries a Linux program links against, for each architecture: the soname and where a
-- distribution keeps the file under its library folder.
local linuxHosts = {
    ["linux-x64"] = {multiarch = "x86_64-linux-gnu", machine = "x86_64",
        libraries = {"libc.so.6", "libm.so.6", "ld-linux-x86-64.so.2", "libgcc_s.so.1"}},
    ["linux-arm64"] = {multiarch = "aarch64-linux-gnu", machine = "aarch64",
        libraries = {"libc.so.6", "libm.so.6", "ld-linux-aarch64.so.1", "libgcc_s.so.1"}},
}

local function numbers(text)
    local found = {}
    for part in text:gmatch("%d+") do
        table.insert(found, tonumber(part))
    end
    return found
end

-- Whether version `a` comes before `b`, both lists of numbers.
local function before(a, b)
    for i = 1, math.max(#a, #b) do
        local x, y = a[i] or 0, b[i] or 0
        if x ~= y then
            return x < y
        end
    end
    return false
end

-- The numbers of a version node at or below the baseline, or nil for one a list leaves out:
-- GLIBC_PRIVATE, the GLIBC_ABI_ markers glibc added after the baseline, and any node newer than
-- the baseline. A node of neither glibc nor GCC stops the script.
local function withinBaseline(node)
    local glibc = node:match("^GLIBC_([%d%.]+)$")
    if glibc then
        local version = numbers(glibc)
        return not before(glibcBaseline, version) and version or nil
    end
    local gcc = node:match("^GCC_([%d%.]+)$")
    if gcc then
        local version = numbers(gcc)
        return not before(gccBaseline, version) and version or nil
    end
    if node == "GLIBC_PRIVATE" or node:startswith("GLIBC_ABI_") then
        return nil
    end
    raise("a library defines the version node '%s', which this script does not know how to "
        .. "compare with the baseline", node)
end

local function readobjProgram()
    local found = os.files(path.join(global.directory(), "packages", "l", "llvm", "*", "*", "bin",
        "llvm-readobj" .. (is_host("windows") and ".exe" or "")))[1]
    if found then
        return found
    end
    local newest = -1
    for _, candidate in ipairs(os.files("/usr/lib/llvm-*/bin/llvm-readobj")) do
        local version = tonumber(candidate:match("llvm%-(%d+)")) or 0
        if version > newest then
            found = candidate
            newest = version
        end
    end
    if found then
        return found
    end
    local onPath = find_tool("llvm-readobj")
    if onPath then
        return onPath.program
    end
    raise("could not find llvm-readobj, which reads the libraries. Run 'xmake build ens-llvm' "
        .. "first, which installs it with the LLVM package, or install LLVM")
end

-- A field of llvm-readobj's JSON as text: one release writes the name alone, another an object
-- holding the name beside its number.
local function named(field)
    if type(field) == "table" then
        field = field.Name
    end
    if type(field) ~= "string" then
        raise("llvm-readobj wrote a symbol field this script cannot read: %s", tostring(field))
    end
    return field
end

-- The entries one library contributes to a list, after its 'library' line.
local function libraryEntries(readobj, file, soname)
    local dump = json.decode(os.iorunv(readobj, {"--elf-output-style=JSON", "--dyn-syms", file}))
    local loadName = dump[1].FileSummary.LoadName
    if loadName ~= soname then
        raise("'%s' names itself '%s', not '%s'", file, tostring(loadName), soname)
    end
    local versions = {}
    local hasDefault = {}
    for _, entry in ipairs(dump[1].DynamicSymbols or {}) do
        local symbol = entry.Symbol
        local full = named(symbol.Name)
        local name, marker, node = full:match("^(.-)(@@?)(.+)$")
        local defined = named(symbol.Section) ~= "Undefined"
        local kind = ({Function = "symbol", GNU_IFunc = "symbol", Object = "object",
            TLS = "tls"})[named(symbol.Type)]
        if name and name ~= node and defined and kind and named(symbol.Binding) ~= "Local" then
            local numbered = withinBaseline(node)
            if marker == "@@" then
                hasDefault[name] = true
            end
            if numbered then
                local chosen = versions[name]
                if not chosen or before(chosen.numbers, numbered) then
                    versions[name] = {numbers = numbered, node = node, kind = kind,
                        size = symbol.Size}
                end
            end
        end
    end
    local lines = {}
    for name, chosen in pairs(versions) do
        if hasDefault[name] then
            if chosen.kind == "symbol" then
                table.insert(lines, string.format("symbol %s %s", name, chosen.node))
            else
                table.insert(lines, string.format("%s %s %s %d", chosen.kind, name, chosen.node,
                    chosen.size))
            end
        end
    end
    table.sort(lines)
    if #lines == 0 then
        raise("'%s' gave no symbols at or below the baseline", file)
    end
    return lines
end

-- What the libraries of `host` export to a program, read from under `root`.
local function linuxList(host, root)
    local described = linuxHosts[host]
    if not described then
        raise("'%s' is not a Linux host a list is kept for. Name 'linux-x64' or 'linux-arm64'", host)
    end
    local readobj = readobjProgram()
    print("reading the libraries under '%s' with '%s'", root, readobj)
    local lines = {
        "# What glibc and libgcc_s export to a " .. described.machine .. " Linux program, at the "
            .. "baselines scripts/refresh_system_symbols.lua names.",
        "# Generated by that script; run it on Linux, or on a folder of a distribution's "
            .. "libraries, instead of editing.",
        "glibc " .. table.concat(glibcBaseline, "."),
    }
    for _, soname in ipairs(described.libraries) do
        local file = nil
        for _, folder in ipairs({path.join("lib", described.multiarch), "lib64", "lib",
                path.join("usr", "lib", described.multiarch)}) do
            local candidate = path.join(root, folder, soname)
            if os.isfile(candidate) then
                file = candidate
                break
            end
        end
        if not file then
            raise("there is no '%s' under '%s'", soname, root)
        end
        table.insert(lines, "library " .. soname)
        for _, line in ipairs(libraryEntries(readobj, file, soname)) do
            table.insert(lines, line)
        end
    end
    return host .. ".symbols", table.concat(lines, "\n") .. "\n"
end

-- The import libraries a Windows program links against, in the order a symbol more than one of
-- them names is taken from: the system's own kernel32 and C runtime first, ntdll last, for what
-- only it exports to a program without the Visual C++ runtime, such as memcpy.
local windowsLibraries = {
    {folder = "um", file = "kernel32.lib"},
    {folder = "ucrt", file = "ucrt.lib"},
    {folder = "um", file = "shell32.lib"},
    {folder = "um", file = "ntdll.lib"},
}

-- The library folder of the Windows SDK xmake's MSVC toolchain finds, which holds a folder for
-- each kind of library and, in each, one for each processor. LIB names other SDKs laid out the same
-- way, such as the .NET Framework's, so the folder taken is the one holding the C runtime's.
local function windowsSdkRoot()
    import("core.tool.toolchain")
    local msvc = toolchain.load("msvc", {plat = "windows", arch = "x64"})
    local lib = msvc and msvc:check() and msvc:runenvs().LIB or ""
    for folder in lib:gmatch("[^;]+") do
        local root = folder:match("^(.-)[/\\]um[/\\]x64[/\\]?$")
        if root and os.isfile(path.join(root, "um", "x64", "kernel32.lib"))
                and os.isfile(path.join(root, "ucrt", "x64", "ucrt.lib")) then
            return root
        end
    end
    raise("could not find the Windows SDK: the MSVC toolchain names no 'um\\x64' library "
        .. "folder. Install the Windows SDK with Visual Studio, or name its library folder after "
        .. "'windows'")
end

-- What one import library names, as {dll = {name = kind}}, where kind is 'symbol' for code and
-- 'data' for data. A member that is not an import, or imports by ordinal alone, names nothing.
local function importsOf(readobj, file)
    local found = {}
    local function take(block)
        if not (block.format or ""):startswith("COFF-import-file") or not block.name then
            return
        end
        local kind = ({code = "symbol", data = "data", const = "data"})[block.type]
        if not kind then
            raise("'%s' imports '%s' as '%s', which this script does not read", file, block.name,
                tostring(block.type))
        end
        found[block.dll] = found[block.dll] or {}
        found[block.dll][block.name] = kind
    end
    local block = {}
    for line in os.iorunv(readobj, {file}):gmatch("[^\r\n]+") do
        local dll = line:match("^File: (.+)$")
        if dll then
            take(block)
            block = {dll = dll}
        end
        block.format = line:match("^Format: (.+)$") or block.format
        block.type = line:match("^Type: (.+)$") or block.type
        block.name = line:match("^Export name: (.+)$") or block.name
    end
    take(block)
    return found
end

-- What the Windows SDK under `root` lets a program for `processor` import, and the SDK's version.
local function windowsList(processor, root)
    local readobj = readobjProgram()
    local taken = {}
    local byDll = {}
    for _, library in ipairs(windowsLibraries) do
        local file = path.join(root, library.folder, processor, library.file)
        if not os.isfile(file) then
            raise("there is no '%s' under '%s'", path.join(library.folder, processor,
                library.file), root)
        end
        for dll, names in pairs(importsOf(readobj, file)) do
            for name, kind in pairs(names) do
                if not taken[name] then
                    taken[name] = true
                    byDll[dll] = byDll[dll] or {}
                    table.insert(byDll[dll], kind .. " " .. name)
                end
            end
        end
    end
    local dlls = {}
    for dll in pairs(byDll) do
        table.insert(dlls, dll)
    end
    table.sort(dlls)
    local version = path.filename(root)
    local lines = {
        "# What a Windows program on " .. processor .. " imports from the system, read from the "
            .. "import libraries of Windows SDK " .. version .. ".",
        "# Generated by scripts/refresh_system_symbols.lua; run it on Windows, or on that SDK's "
            .. "library folder, instead of editing.",
        "sdk " .. version,
    }
    for _, dll in ipairs(dlls) do
        table.insert(lines, "library " .. dll)
        table.sort(byDll[dll])
        for _, line in ipairs(byDll[dll]) do
            table.insert(lines, line)
        end
    end
    if #lines < 1000 then
        raise("the Windows SDK under '%s' gave only %d imports for %s", root, #lines, processor)
    end
    return "windows-" .. processor .. ".symbols", table.concat(lines, "\n") .. "\n"
end

-- The entries of a list, without its comments, as a set.
local function entriesOf(text)
    local found = {}
    for line in (text or ""):gmatch("[^\n]+") do
        if not line:startswith("#") then
            found[line] = true
        end
    end
    return found
end

local function report(name, before, after)
    local added, removed = {}, {}
    for line in pairs(after) do
        if not before[line] then table.insert(added, line) end
    end
    for line in pairs(before) do
        if not after[line] then table.insert(removed, line) end
    end
    table.sort(added)
    table.sort(removed)
    print("%s: %d added, %d removed", name, #added, #removed)
    for _, line in ipairs(added) do print("  + " .. line) end
    for _, line in ipairs(removed) do print("  - " .. line) end
end

function main(host, root)
    local written = {}
    if host then
        if not root then
            raise("'%s' names the host but not the folder holding its libraries. Write the folder "
                .. "after it, as in 'xmake l scripts/refresh_system_symbols.lua %s <folder>'",
                host, host)
        end
        if host == "windows" then
            for _, processor in ipairs({"x64", "arm64"}) do
                local name, text = windowsList(processor, path.absolute(root))
                written[name] = text
            end
        else
            local name, text = linuxList(host, path.absolute(root))
            written[name] = text
        end
    elseif is_host("windows") then
        local root = windowsSdkRoot()
        for _, processor in ipairs({"x64", "arm64"}) do
            local name, text = windowsList(processor, root)
            written[name] = text
        end
    elseif is_host("macosx") then
        local name, text = macosList()
        written[name] = text
    elseif is_host("linux") then
        local host = ({x86_64 = "linux-x64", arm64 = "linux-arm64"})[os.arch()]
        local name, text = linuxList(host or os.arch(), "/")
        written[name] = text
    else
        raise("this machine's system libraries have no list ens links against. Run the script "
            .. "on a Mac, on Linux or on Windows")
    end
    for name, text in pairs(written) do
        local file = path.join(lists, name)
        report(name, entriesOf(os.isfile(file) and io.readfile(file) or ""), entriesOf(text))
        os.mkdir(lists)
        io.writefile(file, text)
    end
end
