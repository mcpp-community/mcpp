#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# #308: a SemVer constraint on a package served by a project `[indices]` entry
# must resolve. Version resolution used to read the shared registry directly
# instead of routing like candidate selection does, so the exact-version form
# (`gadget = "2.0.0"`) worked while the constraint form (`gadget = "^2.0"`) failed
# with "run `mcpp index update` first" — advice that does nothing for a local
# path index. Both forms are asserted here so the two cannot drift apart again.
#
# No network: the index is a local path and the package payload is pre-seeded
# into the project's xlings data dir, so nothing is ever downloaded.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj"
cd "$TMP/proj"

# ── A project-local index carrying acme.gadget 2.0.0 and 2.1.0 ────────────
mkdir -p local-index/pkgs/a
cat > local-index/pkgs/a/acme.gadget.lua <<'EOF'
package = {
    spec = "1",
    namespace = "acme",
    name = "acme.gadget",
    description = "Package reachable only through a project [indices] entry",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["2.0.0"] = {
                url = "https://example.invalid/gadget-2.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["2.1.0"] = {
                url = "https://example.invalid/gadget-2.1.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
        macosx = {
            ["2.0.0"] = {
                url = "https://example.invalid/gadget-2.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["2.1.0"] = {
                url = "https://example.invalid/gadget-2.1.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
        windows = {
            ["2.0.0"] = {
                url = "https://example.invalid/gadget-2.0.0.zip",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["2.1.0"] = {
                url = "https://example.invalid/gadget-2.1.0.zip",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/gadget.cppm" },
        targets = { ["gadget"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# ── Pre-seed the payload so resolution is the only thing under test ─────
mkdir -p .mcpp/.xlings/data/xpkgs/acme.gadget/2.1.0/src
cat > .mcpp/.xlings/data/xpkgs/acme.gadget/2.1.0/src/gadget.cppm <<'EOF'
export module gadget;

export int gadget_value() {
    return 42;
}
EOF

mkdir -p src
cat > src/main.cpp <<'EOF'
import gadget;

int main() {
    return gadget_value() == 42 ? 0 : 1;
}
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[indices]
acme = { path = "local-index" }

[dependencies.acme]
gadget = "^2.0"

[targets.proj]
kind = "bin"
main = "src/main.cpp"
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: SemVer constraint did not resolve against the project [indices] entry"
    exit 1
}

# The constraint must have been pinned to the newest matching version, not
# merely tolerated.
grep -q '→ v2.1.0' build.log || {
    cat build.log
    echo "FAIL: expected '^2.0' to resolve to v2.1.0"
    exit 1
}
grep -q 'index update' build.log && {
    cat build.log
    echo "FAIL: a local path index must never be answered with 'mcpp index update'"
    exit 1
}
grep -q '\[package\."acme.gadget"\]' mcpp.lock || {
    cat mcpp.lock 2>/dev/null || true
    echo "FAIL: expected acme.gadget lock entry"
    exit 1
}
# Only 2.1.0 is seeded above, so a build that got here at all consumed the
# resolved version rather than falling back to the constraint's lower bound.
# (The lockfile records the CONSTRAINT, not the pin — pre-existing behaviour
# shared with registry deps, not something this test is asserting about.)

"$MCPP" run > run.log 2>&1 || { cat run.log; echo "FAIL: run failed"; exit 1; }

# ── The exact-version form keeps working through the same route ─────────
mkdir -p .mcpp/.xlings/data/xpkgs/acme.gadget/2.0.0/src
cp .mcpp/.xlings/data/xpkgs/acme.gadget/2.1.0/src/gadget.cppm \
   .mcpp/.xlings/data/xpkgs/acme.gadget/2.0.0/src/gadget.cppm
sed -i.bak 's/^gadget = "\^2\.0"$/gadget = "2.0.0"/' mcpp.toml && rm -f mcpp.toml.bak
grep -qE '^gadget = "2\.0\.0"$' mcpp.toml || { cat mcpp.toml; echo "FAIL: rewrite to exact version did not apply"; exit 1; }
rm -f mcpp.lock
"$MCPP" build > build2.log 2>&1 || {
    cat build2.log
    echo "FAIL: exact version through a project [indices] entry regressed"
    exit 1
}
grep -q '2\.0\.0' mcpp.lock || { cat mcpp.lock; echo "FAIL: expected 2.0.0 pin"; exit 1; }

echo "OK"
