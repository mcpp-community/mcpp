#!/usr/bin/env bash
# 69_package_templates.sh — package-based `mcpp new --template` (design v2):
# exact multi-level SPEC ([ns.]pkg → default template | pkg:tmpl |
# pkg@ver:tmpl), {{var}} rendering, [template.inject] features,
# --list-templates, sole-template defaults, and provider errors.
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
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        macosx = {
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
            ["1.2.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
            ["1.0.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" },
        },
        windows = {
            ["latest"] = { ref = "1.2.0" },
            ["2.0.0-rc.1"] = { url = "https://example.invalid/x.tar.gz", sha256 = "rc" },
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
seed_pkg 2.0.0-rc.1 rc

# Exact-identity fixtures: a same-short sibling under acme, a nested namespace,
# ambiguous/multiple defaults, and a package that is not a template provider.
write_descriptor() { # path namespace short-name version
    local path="$1" ns="$2" short="$3" ver="$4"
    mkdir -p "$(dirname "$path")"
    cat > "$path" <<EOF
package = {
    spec = "1", namespace = "$ns", name = "$short",
    description = "exact template fixture $ns.$short",
    type = "package",
    xpm = {
        linux   = { ["$ver"] = { url = "https://example.invalid/$short.tar.gz", sha256 = "digest-$short" } },
        macosx  = { ["$ver"] = { url = "https://example.invalid/$short.tar.gz", sha256 = "digest-$short" } },
        windows = { ["$ver"] = { url = "https://example.invalid/$short.zip", sha256 = "digest-$short" } },
    },
    mcpp = "mcpp.toml",
}
EOF
}

seed_template() { # store-dir short version template default marker
    local store="$1" short="$2" ver="$3" tmpl="$4" is_default="$5" marker="$6"
    local root="$MCPP_HOME/registry/data/xpkgs/$store/$ver/$short-$ver"
    mkdir -p "$root/templates/$tmpl"
    cat > "$root/mcpp.toml" <<EOF
[package]
name = "$short"
version = "$ver"
EOF
    cat > "$root/templates/$tmpl/template.toml" <<EOF
[template]
description = "$marker"
EOF
    if [[ "$is_default" == true ]]; then
        echo 'default = true' >> "$root/templates/$tmpl/template.toml"
    fi
    cat > "$root/templates/$tmpl/mcpp.toml.in" <<EOF
[package]
name = "{{project.name}}"
version = "0.1.0"
# fixture-marker=$marker
EOF
}

write_descriptor "$MCPP_HOME/registry/data/mcpplibs/pkgs/a/acme.tpl-demo.lua" \
    acme tpl-demo 3.0.0
seed_template acme-x-tpl-demo tpl-demo 3.0.0 solo false acme-solo

write_descriptor "$MCPP_HOME/registry/data/mcpplibs/pkgs/m/mcpplibs.capi.lua.lua" \
    mcpplibs.capi lua 5.4.7
seed_template mcpplibs.capi-x-lua lua 5.4.7 module false nested-lua

write_descriptor "$MCPP_HOME/registry/data/mcpplibs/pkgs/a/ambiguous.lua" \
    mcpplibs ambiguous 1.0.0
seed_template mcpplibs-x-ambiguous ambiguous 1.0.0 one false ambiguous-one
seed_template mcpplibs-x-ambiguous ambiguous 1.0.0 two false ambiguous-two

write_descriptor "$MCPP_HOME/registry/data/mcpplibs/pkgs/d/dupe-default.lua" \
    mcpplibs dupe-default 1.0.0
seed_template mcpplibs-x-dupe-default dupe-default 1.0.0 one true dupe-one
seed_template mcpplibs-x-dupe-default dupe-default 1.0.0 two true dupe-two

write_descriptor "$MCPP_HOME/registry/data/mcpplibs/pkgs/n/no-templates.lua" \
    mcpplibs no-templates 1.0.0
mkdir -p "$MCPP_HOME/registry/data/xpkgs/mcpplibs-x-no-templates/1.0.0/no-templates-1.0.0"
cat > "$MCPP_HOME/registry/data/xpkgs/mcpplibs-x-no-templates/1.0.0/no-templates-1.0.0/mcpp.toml" <<'EOF'
[package]
name = "no-templates"
version = "1.0.0"
EOF

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

# Explicit prerelease remains selectable, while the omitted version above did
# not promote it over the latest stable release.
"$MCPP" new app2rc --template tpl-demo@2.0.0-rc.1:extra > out2rc.log 2>&1 \
    || { cat out2rc.log; echo "explicit prerelease failed"; exit 1; }
grep -q 'tpl-demo = "2.0.0-rc.1"' app2rc/mcpp.toml \
    || { cat app2rc/mcpp.toml; echo "exact prerelease pin missing"; exit 1; }

# 3. --list-templates shows both, marks the default.
"$MCPP" new --list-templates tpl-demo > list.log 2>&1 || { cat list.log; echo "list failed"; exit 1; }
grep -q "starter" list.log && grep -q "extra" list.log || { cat list.log; echo "missing entries"; exit 1; }
grep -q "(default)" list.log || { cat list.log; echo "default not marked"; exit 1; }

# 4. Unknown template name → error listing alternatives.
if "$MCPP" new app3 --template tpl-demo:nosuch > out4.log 2>&1; then
    echo "unknown template must fail"; exit 1
fi
grep -q "starter" out4.log || { cat out4.log; echo "error must list alternatives"; exit 1; }

# 5. Unknown package → clear exact-identity error, including the default
# namespace that omission selected.
if "$MCPP" new app4 --template no-such-pkg-zz > out5.log 2>&1; then
    echo "unknown package must fail"; exit 1
fi
grep -qi "not found for exact identity" out5.log || { cat out5.log; echo "missing exact index error"; exit 1; }
grep -q "namespace omitted means 'mcpplibs'" out5.log \
    || { cat out5.log; echo "missing default namespace diagnostic"; exit 1; }

# 6. builtin gui still works but prints the deprecation pointer.
"$MCPP" new app6 --template gui > out6.log 2>&1 || { cat out6.log; echo "gui builtin broke"; exit 1; }
grep -qi "deprecated" out6.log || { cat out6.log; echo "missing gui deprecation"; exit 1; }

# 7. Form B (mcpp = { ... }) compat package is selected only when compat is
# explicit, then rejected as a non-template payload. Bare `tpl-repro` must not
# cross from mcpplibs into compat.
mkdir -p "$MCPP_HOME/registry/data/mcpplibs/pkgs/c"
cat > "$MCPP_HOME/registry/data/mcpplibs/pkgs/c/compat.tpl-repro.lua" <<'EOF'
package = {
    spec      = "1",
    namespace = "compat",
    name      = "compat.tpl-repro",
    type      = "package",
    xpm = {
        linux   = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
        macosx  = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
        windows = { ["0.1.0"] = { url = "https://example.invalid/x.tar.gz", sha256 = "0" } },
    },
    mcpp = {
        language     = "c++23",
        include_dirs = {"*"},
        sources      = {"*.cpp"},
        targets      = { tpl_repro = { kind = "lib" } },
    },
}
EOF
mkdir -p "$MCPP_HOME/registry/data/xpkgs/compat-x-compat.tpl-repro/0.1.0"
if "$MCPP" new app7-bare --template tpl-repro > out7-bare.log 2>&1; then
    cat out7-bare.log; echo "L7: bare selector crossed into compat"; exit 1
fi
grep -q "(mcpplibs, tpl-repro)" out7-bare.log \
    || { cat out7-bare.log; echo "L7: bare miss did not name exact identity"; exit 1; }
if "$MCPP" new app7 --template compat.tpl-repro > out7.log 2>&1; then
    cat out7.log; echo "L7: Form B template must fail"; exit 1
fi
grep -qi "inline build recipe" out7.log \
    || { cat out7.log; echo "L7: wrong provider error for Form B"; exit 1; }

# 8. Same short name, explicit foreign namespace: exact IndexRoute lookup must
# select acme.tpl-demo, retain that identity in output/injection, and auto-pick
# its sole non-default template.
"$MCPP" new app-acme --template acme.tpl-demo > out-acme.log 2>&1 \
    || { cat out-acme.log; echo "exact acme template failed"; exit 1; }
grep -q 'fixture-marker=acme-solo' app-acme/mcpp.toml \
    || { cat app-acme/mcpp.toml; echo "default sibling stole acme selector"; exit 1; }
grep -q '^acme\.tpl-demo = "3\.0\.0"$' app-acme/mcpp.toml \
    || { cat app-acme/mcpp.toml; echo "canonical self dependency lost namespace"; exit 1; }
grep -q 'namespace=acme name=tpl-demo' out-acme.log \
    || { cat out-acme.log; echo "resolved output lost PackageId"; exit 1; }
grep -q 'payload=digest-tpl-demo' out-acme.log \
    || { cat out-acme.log; echo "resolved output lost payload provenance"; exit 1; }
grep -q 'acme.tpl-demo@3.0.0:solo' out-acme.log \
    || { cat out-acme.log; echo "sole template did not become default"; exit 1; }

# 9. Nested namespace uses every segment except the final package atom.
"$MCPP" new app-lua --template mcpplibs.capi.lua@5.4.7 > out-lua.log 2>&1 \
    || { cat out-lua.log; echo "nested namespace template failed"; exit 1; }
grep -q 'fixture-marker=nested-lua' app-lua/mcpp.toml \
    || { cat app-lua/mcpp.toml; echo "nested template payload missing"; exit 1; }
grep -q '^mcpplibs\.capi\.lua = "5\.4\.7"$' app-lua/mcpp.toml \
    || { cat app-lua/mcpp.toml; echo "nested PackageId lost during injection"; exit 1; }
grep -q 'namespace=mcpplibs.capi name=lua' out-lua.log \
    || { cat out-lua.log; echo "nested resolved identity missing"; exit 1; }

# 10. Multiple templates without a default are ambiguous; explicit tname is
# deterministic. Multiple defaults and no templates are provider errors.
if "$MCPP" new app-amb --template ambiguous > out-amb.log 2>&1; then
    cat out-amb.log; echo "multiple templates without default must fail"; exit 1
fi
grep -q 'one' out-amb.log && grep -q 'two' out-amb.log \
    || { cat out-amb.log; echo "ambiguous error did not list choices"; exit 1; }
[[ ! -e app-amb ]] || { echo "ambiguous selection created target"; exit 1; }
"$MCPP" new app-explicit --template ambiguous:two > out-explicit.log 2>&1 \
    || { cat out-explicit.log; echo "explicit ambiguous choice failed"; exit 1; }
grep -q 'fixture-marker=ambiguous-two' app-explicit/mcpp.toml \
    || { cat app-explicit/mcpp.toml; echo "wrong explicit template"; exit 1; }

if "$MCPP" new app-dupe --template dupe-default > out-dupe.log 2>&1; then
    cat out-dupe.log; echo "multiple defaults must fail"; exit 1
fi
grep -q 'more than one default' out-dupe.log \
    || { cat out-dupe.log; echo "wrong multiple-default diagnostic"; exit 1; }
if "$MCPP" new app-none --template no-templates > out-none.log 2>&1; then
    cat out-none.log; echo "package without templates must fail"; exit 1
fi
grep -q 'ships no templates' out-none.log \
    || { cat out-none.log; echo "wrong non-provider diagnostic"; exit 1; }

# 11. An explicit version is validated against the descriptor before target
# creation. The legacy trailing-colon list alias warns and redirects users to
# the explicit listing surface for one release train.
if "$MCPP" new app-missing-ver --template tpl-demo@9.9.9 > out-ver.log 2>&1; then
    cat out-ver.log; echo "unpublished exact template version must fail"; exit 1
fi
grep -q 'matches none of' out-ver.log \
    || { cat out-ver.log; echo "missing version inventory diagnostic"; exit 1; }
[[ ! -e app-missing-ver ]] || { echo "missing version created target"; exit 1; }

if "$MCPP" new app-alias --template tpl-demo@latest > out-alias.log 2>&1; then
    cat out-alias.log; echo "moving version alias must not become a resolved template pin"; exit 1
fi
grep -q 'version alias' out-alias.log \
    || { cat out-alias.log; echo "wrong alias diagnostic"; exit 1; }
[[ ! -e app-alias ]] || { echo "version alias created target"; exit 1; }

"$MCPP" new ignored-name --template tpl-demo: > legacy-list.log 2>&1 \
    || { cat legacy-list.log; echo "legacy list alias failed"; exit 1; }
grep -qi 'deprecated' legacy-list.log \
    || { cat legacy-list.log; echo "legacy list alias did not warn"; exit 1; }
grep -q -- '--list-templates tpl-demo' legacy-list.log \
    || { cat legacy-list.log; echo "legacy list warning is not copyable"; exit 1; }
[[ ! -e ignored-name ]] || { echo "legacy list alias created a project"; exit 1; }

# ── L12: the wire address when the package is NOT already installed ─────
#
# Every case above runs off a pre-seeded install cache, so `mcpp new` never
# reaches the install path — which is how that path came to build its xlings
# target as `<ns>.<short>` (a re-derived FQN) unnoticed. That spelling only
# resolved while every descriptor repeated its namespace inside `package.name`;
# the SPEC-001 short-name migration killed it, exactly as it killed the
# dependency path (see e2e 165).
#
# Drop the seeded install and assert on the address that goes out. The xpm url
# is example.invalid, so the install is EXPECTED to fail — the address is the
# thing under test, and checking it separately keeps this honest without
# needing a real payload.
# Assert on the `"targets":[...]` payload specifically, NOT on the log as a
# whole: the human-facing "Downloading …" and "fetch '…' failed" lines carry the
# resolved identity `mcpplibs.tpl-demo`, which is correct there and would make a
# looser grep match no matter what went on the wire.
rm -rf "$MCPP_HOME/registry/data/xpkgs/mcpplibs-x-tpl-demo/1.0.0"
MCPP_VERBOSE=1 "$MCPP" new app8 --template tpl-demo@1.0.0:starter > out8.log 2>&1 || true
# Un-escape first — Windows spells the interface args with backslash-escaped
# quotes; see e2e 165.
sed 's/\\"/"/g' out8.log | grep -oE '"targets":\["[^"]*"\]' > targets8.txt || true
test -s targets8.txt || { echo "L12: no install target was emitted at all"; cat out8.log; exit 1; }
if grep -q '"mcpplibs\.tpl-demo@1\.0\.0"' targets8.txt; then
    echo "L12: install target re-derived the FQN (mcpplibs.tpl-demo) instead of"
    echo "    addressing the descriptor's literal name as mcpplibs:tpl-demo"
    cat targets8.txt
    exit 1
fi
grep -q '"mcpplibs:tpl-demo@1\.0\.0"' targets8.txt || {
    echo "L12: expected the wire address mcpplibs:tpl-demo@1.0.0"
    cat targets8.txt; exit 1; }

echo "OK"
