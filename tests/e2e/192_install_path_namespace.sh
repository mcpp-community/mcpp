#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 192_install_path_namespace.sh — a package must never be satisfied from
# ANOTHER namespace's install directory.
#
# `Fetcher::install_path(ns, shortName, version)` answers "where is THIS
# package installed". Its last-resort legacy scan matched any directory ending
# in `-x-<shortName>`, whatever namespace that directory belonged to, so a
# lookup for `acme:widget@1.5.0` returned `compat-x-widget/1.5.0`. The caller
# then skips the install ("already present") and reads the other package's
# tree.
#
# WHY IT WAS UNREACHABLE UNTIL NOW
#
# install_path also matches on version, so two packages sharing a short name
# collided only if they also shared a version — and the ecosystem's module
# layers carried packaging counters (`imgui@0.0.6`) while the compat packages
# carried upstream versions (`compat.imgui@1.92.8`). Aligning the module layers
# to upstream (mcpp-index#163) makes them coincide, which is how this surfaced.
#
# WHY THE SILENT SHAPE IS THE ONE THAT MATTERS
#
# Discovered against a Form B neighbour, so the wrong verdir had no mcpp.toml
# and the build stopped — with a diagnostic naming the wrong cause. Between two
# packages whose sources live in the verdir, there is no error at all: the
# build compiles the wrong package's source. That is what this test pins.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj"
cd "$TMP/proj"

# ── Two packages, same short name, same version, different namespaces ─────
mkdir -p local-index/pkgs/a
cat > local-index/pkgs/a/acme.widget.lua <<'EOF'
package = {
    spec = "1",
    namespace = "acme",
    name = "widget",
    description = "acme's widget",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.5.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.5.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.5.0"] = { url = "https://example.invalid/w.zip",    sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/widget.cppm" },
        targets = { ["widget"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

# ── Only the FOREIGN namespace's payload is on disk ───────────────────────
# `compat:widget@1.5.0` is installed; `acme:widget@1.5.0` is not. Nothing may
# hand acme's lookup this directory.
#
# It goes in the GLOBAL store ($MCPP_HOME/registry/data/xpkgs), which is what
# `Fetcher::install_path` scans — the project's own .mcpp/.xlings tree is a
# different root reached by `install_path_from_project_data`. Seeding the wrong
# one makes this test pass on a broken binary, which is exactly what the first
# draft did.
mkdir -p "$MCPP_HOME/registry/data/xpkgs/compat-x-widget/1.5.0/src"
cat > "$MCPP_HOME/registry/data/xpkgs/compat-x-widget/1.5.0/src/widget.cppm" <<'EOF'
export module widget;
// If this ever reaches a build that asked for acme:widget, the wrong package
// was compiled. The value is the tell.
export int widget_value() { return 1; }
EOF

mkdir -p src
cat > src/main.cpp <<'EOF'
import widget;
int main() { return widget_value() == 2 ? 0 : 1; }
EOF

cat > mcpp.toml <<'EOF'
[package]
name    = "consumer"
version = "0.1.0"

[dependencies.acme]
widget = "1.5.0"

[indices]
acme = { path = "local-index" }
EOF

# The build must NOT succeed: acme:widget is not installed and its url is
# unreachable by design. What it must never do is satisfy the dependency from
# compat's directory.
if "$MCPP" build > b.log 2>&1; then
    echo "--- build log ---"; cat b.log
    echo "FAIL: build succeeded, so acme:widget was satisfied from somewhere —"
    echo "      the only widget payload on disk belongs to compat."
    exit 1
fi

# Distinguish "correctly refused" from "compiled the wrong package and then
# failed for an unrelated reason": compat's source must never be compiled.
if grep -qE "compat-x-widget/1\.5\.0/src/widget\.cppm" b.log; then
    grep -nE "compat-x-widget" b.log | head -5
    echo "FAIL: the build reached compat's source while resolving acme:widget"
    exit 1
fi
echo "  ok: acme:widget was not satisfied from compat-x-widget"

# And the diagnostic has to name the directory it looked in — `<verdir>` as a
# literal placeholder cannot distinguish a missing mcpp field from a wrong
# verdir, which is what made this take a while to find.
if grep -q "<verdir>" b.log; then
    echo "FAIL: diagnostic still prints the literal placeholder '<verdir>'"
    exit 1
fi
echo "  ok: diagnostics name a real path"

echo "OK"
