#!/usr/bin/env bash
# 204_new_transactional_scaffold.sh — portable names and all-or-nothing new.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

assert_no_stage() {
    local parent=$1
    if find "$parent" -maxdepth 1 -name '.mcpp-new-*' -print -quit \
        | grep -q .; then
        find "$parent" -maxdepth 1 -name '.mcpp-new-*' -print >&2
        fail "transaction staging directory survived"
    fi
}

# Name validation precedes config/index/network activity as well as filesystem
# mutation.  An intentionally missing package must never mask the bad name.
PRE_HOME="$TMP/prevalidation-home"
mkdir -p "$TMP/prevalidation-work"
cd "$TMP/prevalidation-work"
if MCPP_HOME="$PRE_HOME" "$MCPP" new '../escape' --template no-such-package \
    > invalid.log 2>&1; then
    fail "path escape project name was accepted"
fi
grep -qi 'project name\|package selector' invalid.log \
    || { cat invalid.log; fail "invalid-name diagnostic missing"; }
[[ ! -e "$TMP/escape" ]] || fail "path escape created a sibling"
[[ ! -e "$PRE_HOME" ]] || fail "invalid name initialized MCPP_HOME"
assert_no_stage "$TMP/prevalidation-work"

for name in '/absolute' '.' '..' 'a/b' 'a\b' 'CON' 'con.txt' \
            'trail.' 'trail ' 'myPROJECTname' $'bad\tname'; do
    if MCPP_HOME="$PRE_HOME" "$MCPP" new "$name" > invalid.log 2>&1; then
        fail "invalid project name was accepted: [$name]"
    fi
    assert_no_stage "$TMP/prevalidation-work"
done

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
"$MCPP" self config --mirror "${MCPP_E2E_TOOLCHAIN_MIRROR:-GLOBAL}" \
    >/dev/null 2>&1 || true

# Exact foreign-namespace provider with a same-short default-namespace sibling.
INDEX="$MCPP_HOME/registry/data/mcpplibs/pkgs/t"
mkdir -p "$INDEX"
for row in 'acme tx-template acme-x-tx-template 2.0.0' \
           'mcpplibs tx-template mcpplibs-x-tx-template 9.0.0'; do
    set -- $row
    ns=$1 short=$2 store=$3 version=$4
    cat > "$INDEX/${ns}.${short}.lua" <<EOF
package = {
    spec = "1", namespace = "$ns", name = "$short",
    description = "transaction scaffold fixture",
    type = "package",
    xpm = {
        linux   = { ["$version"] = { url = "https://example.invalid/$short.tar.gz", sha256 = "$ns-digest" } },
        macosx  = { ["$version"] = { url = "https://example.invalid/$short.tar.gz", sha256 = "$ns-digest" } },
        windows = { ["$version"] = { url = "https://example.invalid/$short.zip", sha256 = "$ns-digest" } },
    },
    mcpp = "mcpp.toml",
}
EOF
    root="$MCPP_HOME/registry/data/xpkgs/$store/$version/$short-$version"
    mkdir -p "$root/templates/starter/src"
    cat > "$root/mcpp.toml" <<EOF
[package]
name = "$short"
version = "$version"
EOF
    cat > "$root/templates/starter/template.toml" <<'EOF'
[template]
description = "single template is implicitly default"

[template.inject]
self = { features = ["gui"] }
EOF
    cat > "$root/templates/starter/mcpp.toml.in" <<'EOF'
[package]
name = "{{project.qualifiedName}}"
version = "0.1.0"

[dependencies.compat]
tx-template = "1.0.0"

# project={{project.name}} ns={{project.namespace}} qualified={{project.qualifiedName}}
# provider={{template.package.namespace}}/{{template.package.name}}
# selector={{template.package.selector}} version={{template.package.version}}
# template={{template.name}}
EOF
    cat > "$root/templates/starter/src/main.cpp.in" <<'EOF'
// {{project.qualifiedName}} from {{template.package.selector}}@{{template.package.version}}:{{template.name}}
int main() { return 0; }
EOF
done

WORK="$TMP/work"
mkdir -p "$WORK"
cd "$WORK"

# Builtin scaffolding commits only after every file and manifest validate.
"$MCPP" new acme.tools.app > builtin.log 2>&1 \
    || { cat builtin.log; fail "builtin scaffold failed"; }
[[ -f acme.tools.app/mcpp.toml && -f acme.tools.app/src/main.cpp \
   && -f acme.tools.app/tests/test_smoke.cpp ]] \
    || fail "builtin scaffold is incomplete"
grep -q 'name        = "acme.tools.app"' acme.tools.app/mcpp.toml \
    || fail "qualified project identity missing from builtin manifest"
grep -q 'Hello from app!' acme.tools.app/src/main.cpp \
    || fail "builtin project.name token did not use the short name"
assert_no_stage "$WORK"

# Valid package scaffold preserves every identity component and injects the
# exact acme PackageId despite a compat package with the same short name.
"$MCPP" new org.demo.app --template acme.tx-template@2.0.0 \
    > package.log 2>&1 || { cat package.log; fail "package scaffold failed"; }
grep -q 'project=app ns=org.demo qualified=org.demo.app' \
    org.demo.app/mcpp.toml || fail "project RenderVars are incomplete"
grep -q 'provider=acme/tx-template' org.demo.app/mcpp.toml \
    || fail "provider PackageId was lost"
grep -q 'selector=acme.tx-template version=2.0.0' org.demo.app/mcpp.toml \
    || fail "resolved selector/version were lost"
grep -q 'template=starter' org.demo.app/mcpp.toml \
    || fail "implicit single-template selection was not rendered"
grep -q '^\[dependencies\.acme\]$' org.demo.app/mcpp.toml \
    && grep -q '^tx-template = { version = "2.0.0", features = \["gui"\] }$' \
        org.demo.app/mcpp.toml \
    || { cat org.demo.app/mcpp.toml; fail "exact self dependency missing"; }
grep -q '^tx-template = "1.0.0"$' org.demo.app/mcpp.toml \
    || fail "same-short compat dependency was overwritten"
assert_no_stage "$WORK"

# Add failure templates only after proving that a sole template with no
# `default = true` is selected automatically.
PKG="$MCPP_HOME/registry/data/xpkgs/acme-x-tx-template/2.0.0/tx-template-2.0.0"
mkdir -p "$PKG/templates/broken" "$PKG/templates/collision/a"
cat > "$PKG/templates/broken/template.toml" <<'EOF'
[template]
description = "unknown token rollback"
EOF
cat > "$PKG/templates/broken/mcpp.toml.in" <<'EOF'
[package]
name = "{{project.typo}}"
version = "0.1.0"
EOF
cat > "$PKG/templates/collision/template.toml" <<'EOF'
[template]
description = "destination type collision rollback"
EOF
cat > "$PKG/templates/collision/mcpp.toml.in" <<'EOF'
[package]
name = "{{project.name}}"
version = "0.1.0"
EOF
# Whichever source entry is visited first, `a.in` rendering and directory `a`
# cannot both materialize at destination path `a`.
echo data > "$PKG/templates/collision/a.in"
echo nested > "$PKG/templates/collision/a/member.txt"

# Unknown render tokens and destination type conflicts roll back the complete
# staging tree and never publish the final project directory.
for case_name in broken collision; do
    target="fail-$case_name"
    if "$MCPP" new "$target" \
        --template "acme.tx-template@2.0.0:$case_name" \
        > "$case_name.log" 2>&1; then
        cat "$case_name.log"
        fail "$case_name failure template unexpectedly succeeded"
    fi
    [[ ! -e "$target" ]] || fail "$case_name left a partial final target"
    assert_no_stage "$WORK"
done
grep -q 'unknown template token' broken.log \
    || { cat broken.log; fail "unknown-token diagnostic missing"; }
grep -qi 'failed\|cannot\|directory' collision.log \
    || { cat collision.log; fail "destination-collision diagnostic missing"; }

# Symlink payload rejection is exercised where the host permits symlinks. It
# is intentionally not a hard capability requirement so portable-name and
# renderer transaction coverage still runs on restricted Windows runners.
mkdir -p "$PKG/templates/symlinked"
cat > "$PKG/templates/symlinked/template.toml" <<'EOF'
[template]
description = "symlink rollback"
EOF
cat > "$PKG/templates/symlinked/mcpp.toml.in" <<'EOF'
[package]
name = "{{project.name}}"
version = "0.1.0"
EOF
symlink_path="$PKG/templates/symlinked/escape-link"
if ln -s mcpp.toml.in "$symlink_path" 2>/dev/null \
    && [[ -L "$symlink_path" ]]; then
    if "$MCPP" new fail-symlink \
        --template acme.tx-template@2.0.0:symlinked \
        > symlink.log 2>&1; then
        cat symlink.log
        fail "symlink template unexpectedly succeeded"
    fi
    grep -qi 'symlink' symlink.log \
        || { cat symlink.log; fail "symlink rejection diagnostic missing"; }
    [[ ! -e fail-symlink ]] || fail "symlink failure left final target"
    assert_no_stage "$WORK"
else
    # Some restricted Windows shells report success from `ln -s` without
    # creating a symbolic link. Do not turn that environment quirk into a
    # false assertion about scaffold behavior.
    rm -f -- "$symlink_path"
fi

echo OK
