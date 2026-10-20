-- Rewrites the symbol lists ens links a program's system libraries against, from the libraries of
-- the machine it runs on: `xmake l scripts/refresh_system_symbols.lua`. Only this machine's own lists
-- are rewritten, and what was added and removed is printed. Input it cannot read stops it before
-- anything is written, so a list never comes out shorter than what the platform provides.
--
-- A list is text of our own, one entry per line, read by selfhost/bundled/src/symbols.ens:
--   sdk <version>                    the version of the SDK the list was read from
--   library <install name> <version> the library a program records it links against
--   symbol <name>                    a symbol the library exports
--   weak-symbol <name>               a symbol the library exports as a weak definition
-- Lines starting with '#' are comments.

import("core.base.json")

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

function main()
    local written = {}
    if is_host("macosx") then
        local name, text = macosList()
        written[name] = text
    else
        raise("this machine's system libraries have no list ens links against yet. Run the "
            .. "script on a Mac to refresh the macOS list")
    end
    for name, text in pairs(written) do
        local file = path.join(lists, name)
        report(name, entriesOf(os.isfile(file) and io.readfile(file) or ""), entriesOf(text))
        os.mkdir(lists)
        io.writefile(file, text)
    end
end
