#!/usr/bin/env bash
# 69_package_templates.sh — package-based `mcpp new --template` (design v2):
# multi-level SPEC (bare pkg → default template | pkg:tmpl | pkg@ver:tmpl),
# {{var}} rendering, [template.inject] features, --list-templates, errors.
#
# Hermetic: the package is pre-seeded into the home's install cache and the
# index lua is written into the local official-index clone — no network.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
"$MCPP" self config --mirror "${MCPP_E2E_TOOLCHAIN_MIRROR:-GLOBAL}" >/dev/null 2>&1 || true

# ── index entry (local official-index clone) ────────────────────────────
mkdir -p "$MCPP_HOME/registry/data/mcpplibs/pkgs/t"
cat > "$MCPP_HOME/registry/data/mcpplibs/pkgs/t/tpl-demo.lua" <<'EOF'
package = {
    spec      = "1",
    namespace = "mcpplibs",
    name      = "tpl-demo",
    description = "template e2e fixture",
    type      = "package",
    xpm = {
        linux = {
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        macosx = {
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        windows = {
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
    },
    mcpp = "src/*.cppm",
}
EOF

# ── pre-seeded installed package (both versions) ────────────────────────
seed_pkg() { # version, marker
    local ver="$1" marker="$2"
    local root="$MCPP_HOME/registry/data/xpkgs/mcpplibs-x-tpl-demo/$ver/tpl-demo-$ver"
    mkdir -p "$root/src" "$root/templates/starter/src" "$root/templates/extra"
    cat > "$root/mcpp.toml" <<EOF
[package]
name    = "tpl-demo"
version = "$ver"
EOF
    echo "export module tpldemo;" > "$root/src/lib.cppm"

    cat > "$root/templates/starter/template.toml" <<EOF
[template]
description  = "starter template ($marker)"
default      = true
post_message = "post-message-marker-$marker"

[template.inject]
self = { features = ["alpha", "beta"] }
EOF
    cat > "$root/templates/starter/mcpp.toml.in" <<'EOF'
[package]
name    = "{{project.name}}"
version = "0.1.0"
# from {{self.name}} {{self.version}}
EOF
    cat > "$root/templates/starter/src/main.cpp.in" <<'EOF'
// {{project.name}} via {{self.name}}@{{self.version}}
import std;
int main() { std::println("{{project.name}}"); return 0; }
EOF
    echo "static-data" > "$root/templates/starter/NOTES.md"

    cat > "$root/templates/extra/template.toml" <<EOF
[template]
description = "secondary template"
EOF
    cat > "$root/templates/extra/mcpp.toml.in" <<'EOF'
[package]
name    = "{{project.name}}"
version = "0.1.0"

[dependencies]
tpl-demo = "{{self.version}}"
EOF
}
seed_pkg 1.0.0 old
seed_pkg 1.2.0 new

WORK="$TMP/work"; mkdir -p "$WORK"; cd "$WORK"

# 1. L0 bare package name → default template, latest version (1.2.0).
"$MCPP" new app1 --template tpl-demo > out1.log 2>&1 || { cat out1.log; echo "L0 failed"; exit 1; }
grep -q "tpl-demo@1.2.0:starter" out1.log || { cat out1.log; echo "missing resolved spec"; exit 1; }
grep -q "post-message-marker-new" out1.log || { cat out1.log; echo "missing post_message"; exit 1; }
grep -q 'name    = "app1"' app1/mcpp.toml || { echo "project.name not rendered"; exit 1; }
grep -q "from tpl-demo 1.2.0" app1/mcpp.toml || { echo "self.* not rendered"; exit 1; }
# inject with features (template did not declare the dep itself)
grep -q 'tpl-demo = { version = "1.2.0", features = \["alpha", "beta"\] }' app1/mcpp.toml \
    || { cat app1/mcpp.toml; echo "inject(features) missing"; exit 1; }
[[ -f app1/NOTES.md ]] || { echo "verbatim copy missing"; exit 1; }
[[ ! -f app1/template.toml ]] || { echo "template.toml must not be copied"; exit 1; }
grep -q "app1 via tpl-demo@1.2.0" app1/src/main.cpp || { echo "main.cpp not rendered"; exit 1; }

# 2. L3 fully explicit: pinned version + named template; template declares
#    the dep itself via {{self.version}} → no duplicate injection.
"$MCPP" new app2 --template tpl-demo@1.0.0:extra > out2.log 2>&1 || { cat out2.log; echo "L3 failed"; exit 1; }
grep -q 'tpl-demo = "1.0.0"' app2/mcpp.toml || { cat app2/mcpp.toml; echo "self.version pin missing"; exit 1; }
n=$(grep -c "tpl-demo" app2/mcpp.toml); [[ "$n" -eq 1 ]] || { cat app2/mcpp.toml; echo "duplicate injection"; exit 1; }

# 3. --list-templates shows both, marks the default.
"$MCPP" new --list-templates tpl-demo > list.log 2>&1 || { cat list.log; echo "list failed"; exit 1; }
grep -q "starter" list.log && grep -q "extra" list.log || { cat list.log; echo "missing entries"; exit 1; }
grep -q "(default)" list.log || { cat list.log; echo "default not marked"; exit 1; }

# 4. Unknown template name → error listing alternatives.
if "$MCPP" new app3 --template tpl-demo:nosuch > out4.log 2>&1; then
    echo "unknown template must fail"; exit 1
fi
grep -q "starter" out4.log || { cat out4.log; echo "error must list alternatives"; exit 1; }

# 5. Unknown package → clear index error.
if "$MCPP" new app4 --template no-such-pkg-zz > out5.log 2>&1; then
    echo "unknown package must fail"; exit 1
fi
grep -qi "not found in the index" out5.log || { cat out5.log; echo "missing index error"; exit 1; }

# 6. builtin gui still works but prints the deprecation pointer.
"$MCPP" new app6 --template gui > out6.log 2>&1 || { cat out6.log; echo "gui builtin broke"; exit 1; }
grep -qi "deprecated" out6.log || { cat out6.log; echo "missing gui deprecation"; exit 1; }

echo "OK"
