-- Writes the release version into the tree: `xmake l scripts/set_version.lua 0.10.0-beta`.
-- The release workflow runs it on every host before building, so every binary reports the version
-- it is published under, and the commit the release is tagged on holds the same edit.
function main(version)
    if not version then
        raise("no version given. Write it after the script, as in "
            .. "'xmake l scripts/set_version.lua 0.10.0-beta'")
    end
    if not version:match("^%d+%.%d+%.%d+$") and not version:match("^%d+%.%d+%.%d+%-[%w%.]+$") then
        raise("'%s' is not a release version. Write major.minor.patch, optionally followed by "
            .. "a label such as '-beta', as in '0.10.0-beta'", version)
    end
    local file = path.join(os.scriptdir(), "..", "selfhost", "language", "src", "version.ens")
    local text = io.readfile(file)
    local pattern = '(export static const string release = ")[^"]*(";)'
    local updated, count = text:gsub(pattern, "%1" .. version .. "%2")
    if count ~= 1 then
        raise("'%s' does not declare 'release' once, so the version cannot be written into it",
            file)
    end
    io.writefile(file, updated)
end
