#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 990 -- a lock entry's key and hash do not depend on which members a command
# plans (#868).
#
# Two members take one workspace dependency under two spellings: `a` writes
# `widget2.workspace = true`, `c` writes `[dependencies.mcpplibs]
# widget2.workspace = true`. The key was the spelling of a selected member, so
# `-p a` wrote `[package."widget2"]` and `-p c` `[package."mcpplibs.widget2"]`,
# each with its own hash (the hash is taken over the key), and the lock changed
# between commands with no change to a manifest.
#
# Criteria: `--workspace`, `-p a` and `-p c` write the same key and hash; it is
# the workspace's own spelling (`[workspace.dependencies] widget2`), which
# precedes every member's.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"

# A package of the default namespace, from a project-local index, with its
# payload already in place: nothing is fetched.
mkdir -p local-index/pkgs/w .mcpp/.xlings/data/xpkgs/mcpplibs.widget2/1.0.0/src
cat > local-index/pkgs/w/widget2.lua <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "widget2",
    description = "a package of the default namespace from a local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/w.zip" } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/widget2.cppm" },
        targets = { ["widget2"] = { kind = "lib" } },
        deps = {},
    },
}
EOF
printf 'export module widget2;\nexport int widget2_value() { return 35; }\n' \
    > .mcpp/.xlings/data/xpkgs/mcpplibs.widget2/1.0.0/src/widget2.cppm

cat > mcpp.toml <<'EOF'
[workspace]
members = ["a", "c"]

[workspace.indices]
mcpplibs = { path = "local-index" }

[workspace.dependencies]
widget2 = "1.0.0"
EOF

mkdir -p a/src c/src
cat > a/mcpp.toml <<'EOF'
[package]
name = "a"
version = "0.1.0"

[dependencies]
widget2.workspace = true

[targets.a]
kind = "lib"
EOF
printf 'export module a;\nimport widget2;\nexport int a_value() { return widget2_value() + 7; }\n' > a/src/a.cppm

cat > c/mcpp.toml <<'EOF'
[package]
name = "c"
version = "0.1.0"

[dependencies.mcpplibs]
widget2.workspace = true

[targets.c]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'import widget2;\nint main() { return widget2_value() == 35 ? 0 : 1; }\n' > c/src/main.cpp

entry() {  # the lock's widget2 entry: its header and its hash
    grep -E '^\[package\..*widget2"\]' mcpp.lock
    awk '/widget2"\]/{f=1} f && /^hash/{print; exit}' mcpp.lock
}

declare -A seen
for sel in "--workspace" "-p a" "-p c"; do
    rm -f mcpp.lock
    # shellcheck disable=SC2086
    "$MCPP" build $sel --configure-only > "build.log" 2>&1 || {
        cat build.log
        echo "FAIL: mcpp build $sel --configure-only"
        exit 1
    }
    seen[$sel]="$(entry)"
done

for sel in "--workspace" "-p a" "-p c"; do
    [[ "${seen[$sel]}" == "${seen[--workspace]}" ]] || {
        for s in "--workspace" "-p a" "-p c"; do echo "$s: ${seen[$s]}"; done
        echo "FAIL: the lock entry of widget2 depends on the members a command plans"
        exit 1
    }
done
grep -q '^\[package\."widget2"\]' <<<"${seen[--workspace]}" || {
    echo "${seen[--workspace]}"
    echo "FAIL: the key is not the workspace's own spelling (widget2)"
    exit 1
}

echo "OK"
