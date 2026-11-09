#!/usr/bin/env bash
# Writes the release version into the tree: `scripts/set_version.sh 0.10.0-beta`.
# The release workflow runs it on every host before building, so every binary reports the version
# it is published under, and the commit the release is tagged on holds the same edit.
# It needs nothing but bash and awk, so the job that only writes the version and opens the draft
# does not install xmake.
set -euo pipefail

version="${1-}"
if [[ -z $version ]]; then
    echo "no version given. Write it after the script, as in" \
        "'scripts/set_version.sh 0.10.0-beta'" >&2
    exit 1
fi
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$ ]]; then
    echo "'$version' is not a release version. Write major.minor.patch, optionally followed by a" \
        "label such as '-beta', as in '0.10.0-beta'" >&2
    exit 1
fi

folder="$(cd "$(dirname "$0")/../selfhost/language/src" && pwd)"
file="$folder/version.ens"
updated="$(mktemp)"
trap 'rm -f "$updated"' EXIT
if ! awk -v version="$version" '
    /^    export static const string release = "/ {
        print "    export static const string release = \"" version "\";"
        found++
        next
    }
    { print }
    END { exit found == 1 ? 0 : 1 }
' "$file" > "$updated"; then
    echo "'$file' does not declare 'release' once, so the version cannot be written into it" >&2
    exit 1
fi
mv "$updated" "$file"
