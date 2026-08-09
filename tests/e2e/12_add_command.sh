#!/usr/bin/env bash
# requires:
# `mcpp add` modifies mcpp.toml [dependencies]. Default-namespace packages land
# as bare keys under [dependencies]; canonical `ns.name` inputs use a subtable;
# the deprecated `ns:name` spelling is accepted only as a migration alias. A
# package no readable index can serve is
# refused before mcpp.toml is touched (#305) — but only where absence is
# provable, so the gate resolves the same candidates the manifest parser derives
# and reads through the same index routing `mcpp build` resolves dependencies
# with.
#
# Every package here is served by a project-local `[indices] path = …`, so the
# assertions depend on neither the network nor the state of the shared registry.
# MCPP_HOME is deliberately NOT isolated: an empty home would force a full
# sandbox bootstrap (xlings, index fetch, patchelf, ninja) on a test that is
# otherwise pure local file manipulation.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
TMP_HOST="$(host_path "$TMP")"
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# A local index carrying five packages: two in the DEFAULT namespace, one under
# `acme`, and a same-short-name pair under `capi` / `mcpplibs.capi` that proves
# exact dotted selection cannot be stolen by the old prefixed candidate.
# Descriptors are filed under the first letter of their FILENAME, and publish
# every OS so the version assertions below hold on all three CI platforms.
mkdir -p index/pkgs/w index/pkgs/g index/pkgs/a index/pkgs/c index/pkgs/m
cat > index/pkgs/w/widget.lua <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "widget",
    description = "default-namespace package served by a local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["0.1.0"] = { url = "https://example.invalid/w.tar.gz" } },
        macosx  = { ["0.1.0"] = { url = "https://example.invalid/w.tar.gz" } },
        windows = { ["0.1.0"] = { url = "https://example.invalid/w.zip" } },
    },
}
EOF
cat > index/pkgs/g/gadget.lua <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "gadget",
    description = "second default-namespace package",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["0.2.0"] = { url = "https://example.invalid/g.tar.gz" } },
        macosx  = { ["0.2.0"] = { url = "https://example.invalid/g.tar.gz" } },
        windows = { ["0.2.0"] = { url = "https://example.invalid/g.zip" } },
    },
}
EOF
cat > index/pkgs/a/acme.util.lua <<'EOF'
package = {
    spec = "1",
    namespace = "acme",
    name = "util",
    description = "nested-namespace package served by a local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        macosx  = { ["2.0.0"] = { url = "https://example.invalid/u.tar.gz" } },
        windows = { ["2.0.0"] = { url = "https://example.invalid/u.zip" } },
    },
}
EOF
cat > index/pkgs/c/capi.lua.lua <<'EOF'
package = {
    spec = "1",
    namespace = "capi",
    name = "lua",
    description = "new exact dotted identity",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["5.4.7"] = { url = "https://example.invalid/lua.tar.gz" } },
        macosx  = { ["5.4.7"] = { url = "https://example.invalid/lua.tar.gz" } },
        windows = { ["5.4.7"] = { url = "https://example.invalid/lua.zip" } },
    },
}
EOF
cat > index/pkgs/m/mcpplibs.capi.lua.lua <<'EOF'
package = {
    spec = "1",
    namespace = "mcpplibs.capi",
    name = "lua",
    description = "old prefixed dotted candidate",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["9.9.9"] = { url = "https://example.invalid/old.tar.gz" } },
        macosx  = { ["9.9.9"] = { url = "https://example.invalid/old.tar.gz" } },
        windows = { ["9.9.9"] = { url = "https://example.invalid/old.zip" } },
    },
}
EOF
cat >> mcpp.toml <<'EOF'

[indices]
default = { path = "index" }
acme = { path = "index" }
capi = { path = "index" }
EOF

# (1) Default-namespace dep: bare name → unquoted key under [dependencies].
"$MCPP" add widget@0.1.0 > /dev/null
grep -qE '^\[dependencies\]'    mcpp.toml || { cat mcpp.toml; echo "no [dependencies] section"; exit 1; }
grep -qE '^widget = "0\.1\.0"$' mcpp.toml || { cat mcpp.toml; echo "widget entry missing or quoted"; exit 1; }
grep -qE '^"widget"'            mcpp.toml && { cat mcpp.toml; echo "default-ns key should not be quoted"; exit 1; }

# (2) `<ns>.<name>` where ns IS the default (mcpplibs) — still a bare key under
# [dependencies], NOT [dependencies.mcpplibs]. Appends without duplicating the
# section header.
"$MCPP" add mcpplibs.gadget@0.2.0 > /dev/null
header_count=$(grep -cE '^\[dependencies\]$' mcpp.toml)
[[ "$header_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies] header duplicated"; exit 1; }
grep -qE '^gadget = "0\.2\.0"$' mcpp.toml || { cat mcpp.toml; echo "gadget not set"; exit 1; }

# (3) A dotted selector is one exact identity and is stored canonically as a
# namespace subtable. It must never become an ordered fallback search.
"$MCPP" add acme.util@2.0.0 > /dev/null
grep -qE '^\[dependencies\.acme\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.acme] section"; exit 1; }
grep -qE '^util = "2\.0\.0"$'       mcpp.toml || { cat mcpp.toml; echo "util entry missing"; exit 1; }
! grep -qE '^acme\.util = '          mcpp.toml || { cat mcpp.toml; echo "dotted selector must not be stored as an ambiguous flat key"; exit 1; }

# (4) The old colon spelling remains a one-release migration alias, gives a
# copyable canonical replacement, and writes the same canonical shape.
alias_out=$("$MCPP" add acme:util@2.0.0 2>&1)
[[ "$alias_out" == *"deprecated"* ]]       || { echo "missing migration warning: $alias_out"; exit 1; }
[[ "$alias_out" == *"acme.util@2.0.0"* ]] || { echo "warning lacks canonical replacement: $alias_out"; exit 1; }

# (5) `capi.lua` is exactly (capi,lua), even though the old first candidate
# (mcpplibs.capi,lua) exists with the requested 9.9.9 version. The version
# warning must come from capi:lua and the migration warning shows both complete
# selectors without silently selecting the sibling.
exact_out=$("$MCPP" add capi.lua@9.9.9 2>&1)
[[ "$exact_out" == *"available: 5.4.7"* ]]       || { echo "sibling stole exact selector: $exact_out"; exit 1; }
[[ "$exact_out" == *"mcpplibs.capi.lua"* ]]     || { echo "migration warning lacks old selector: $exact_out"; exit 1; }
[[ "$exact_out" == *"capi.lua"* ]]              || { echo "migration warning lacks new selector: $exact_out"; exit 1; }
"$MCPP" add capi.lua@5.4.7 > /dev/null
grep -qE '^\[dependencies\.capi\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.capi] section"; exit 1; }
grep -qE '^lua = "5\.4\.7"$'             mcpp.toml || { cat mcpp.toml; echo "exact capi.lua entry missing"; exit 1; }

# Nested namespaces use the same parser for add and remove.
"$MCPP" add mcpplibs.capi.lua@9.9.9 > /dev/null
grep -qE '^\[dependencies\.mcpplibs\.capi\]$' mcpp.toml || { cat mcpp.toml; echo "nested namespace section missing"; exit 1; }
"$MCPP" remove mcpplibs.capi.lua > /dev/null
! grep -qE '^lua = "9\.9\.9"$' mcpp.toml || { cat mcpp.toml; echo "nested exact remove failed"; exit 1; }

# (6) Dotted remove can still clean the old subtable shape for compatibility.
cat >> mcpp.toml <<'EOF'

[dependencies.legacy]
old = "0.1.0"
EOF
"$MCPP" remove legacy.old > /dev/null
! grep -qE '^old = "0\.1\.0"$' mcpp.toml || { cat mcpp.toml; echo "legacy.old was not removed"; exit 1; }

# (7) Reject missing version.
err=$("$MCPP" add bareword 2>&1) && { echo "expected error for missing version"; exit 1; }
[[ "$err" == *"version required"* ]] || { echo "wrong error: $err"; exit 1; }

# (8) Reject empty package name (e.g. `mcpp add :@1.0`).
err=$("$MCPP" add ":@1.0" 2>&1) && { echo "expected error for empty package name"; exit 1; }

# (9) Reject malformed/unsafe selectors before any index lookup or manifest
# mutation. These spellings must not be normalized into a different identity.
cp mcpp.toml "$TMP/before-invalid"
for invalid in 'ocornut..imgui@1.0.0' 'acme/util@1.0.0' 'acme\util@1.0.0' 'acme.util@' 'acme.util@1.0@2.0' 'widget@1.0"bad'; do
    err=$("$MCPP" add "$invalid" 2>&1) && { echo "expected error for invalid selector: $invalid"; exit 1; }
    [[ "$err" == *"invalid"* || "$err" == *"version required"* ]] || { echo "wrong error for $invalid: $err"; exit 1; }
    diff -q "$TMP/before-invalid" mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for invalid selector: $invalid"; exit 1; }
done

# (10) A package the index does not carry is refused, and mcpp.toml is left
# exactly as it was. The error names the identities that were tried.
cp mcpp.toml "$TMP/before"
err=$("$MCPP" add definitely-not-a-real-package@9.9.9 2>&1) && { echo "expected error for missing package"; exit 1; }
[[ "$err" == *"not found"* ]] || { echo "wrong error: $err"; exit 1; }
[[ "$err" == *"tried:"*    ]] || { echo "error should list the identities tried: $err"; exit 1; }
diff -q "$TMP/before" mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for missing package"; exit 1; }

# (11) Same for an explicitly-namespaced miss in a readable index.
err=$("$MCPP" add acme.nope@1.0.0 2>&1) && { echo "expected error for missing acme package"; exit 1; }
[[ "$err" == *"(acme, nope)"* ]] || { echo "wrong error: $err"; exit 1; }
[[ "$err" == *"route: local index 'acme': root present, pkgs present"* ]] || {
    echo "missing privacy-safe local-index route state: $err"
    exit 1
}
diff -q "$TMP/before" mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for missing package"; exit 1; }

# (12) A namespace no readable index covers cannot be refuted, so the add goes
# through unverified rather than failing. Refusing here would be worse than the
# bug being fixed — `mcpp build` resolves such dependencies fine.
"$MCPP" add unknownidx.thing@1.0.0 > /dev/null
grep -qE '^\[dependencies\.unknownidx\]$' mcpp.toml || { cat mcpp.toml; echo "unverifiable namespace should still be added"; exit 1; }

# (13) A real package asked for at a version it does not publish is a warning,
# not a refusal — version tables are per-OS, so "absent here" is not "absent".
out=$("$MCPP" add widget@9.9.9 2>&1)
[[ "$out" == *"available: 0.1.0"* ]] || { echo "expected available-versions warning: $out"; exit 1; }
grep -qE '^widget = "9\.9\.9"$' mcpp.toml || { cat mcpp.toml; echo "widget@9.9.9 should still be written"; exit 1; }

# (14) A workspace MEMBER inherits the root's [indices], so adding inside it
# sees the same packages `mcpp build` would resolve from there (#224).
mkdir -p "$TMP/ws"
cd "$TMP/ws"
cp -r "$TMP/myapp/index" index
[[ -f index/pkgs/a/acme.util.lua ]] || {
    echo "workspace index fixture copy is incomplete"
    exit 1
}
cat > mcpp.toml <<'EOF'
[workspace]
members = ["m1"]

[indices]
acme = { path = "index" }
EOF
"$MCPP" new m1 > /dev/null
cd m1
workspace_add=$("$MCPP" add acme.util@2.0.0 2>&1) || {
    echo "$workspace_add"
    echo "workspace member could not read its root-owned local index"
    exit 1
}
grep -qE '^util = "2\.0\.0"$' mcpp.toml || { cat mcpp.toml; echo "member should inherit workspace [indices]"; exit 1; }
err=$("$MCPP" add acme.nope@1.0.0 2>&1) && { echo "expected error inside workspace member"; exit 1; }
[[ "$err" == *"(acme, nope)"* ]] || { echo "wrong error: $err"; exit 1; }

# (15) Adding an exact selector upserts an equivalent legacy flat spelling
# instead of leaving two source forms for one PackageId.
mkdir -p "$TMP/upsert"
cat > "$TMP/upsert/mcpp.toml" <<EOF
[package]
name = "upsert"
version = "0.1.0"

[indices]
acme = { path = "$TMP_HOST/myapp/index" }

[dependencies]
"acme.util" = "1.0.0"
EOF
cd "$TMP/upsert"
"$MCPP" add acme.util@2.0.0 > /dev/null
! grep -qE '^"acme\.util" = ' mcpp.toml || {
    cat mcpp.toml
    echo "legacy flat selector was duplicated instead of migrated"
    exit 1
}
[[ $(grep -cE '^util = "2\.0\.0"$' mcpp.toml) == 1 ]] || {
    cat mcpp.toml
    echo "canonical exact selector was not upserted once"
    exit 1
}

# (16) Exact removal is scoped to [dependencies]. A same-name dev dependency
# that appears earlier in the file must not be removed by a global text match.
cat > mcpp.toml <<'EOF'
[package]
name = "remove-scope"
version = "0.1.0"

[dev-dependencies]
widget = "0.2.0"

[dependencies]
widget = "0.1.0"
EOF
"$MCPP" remove widget > /dev/null
grep -qE '^widget = "0\.2\.0"$' mcpp.toml || {
    cat mcpp.toml
    echo "remove escaped [dependencies] and deleted the dev dependency"
    exit 1
}
! grep -qE '^widget = "0\.1\.0"$' mcpp.toml || {
    cat mcpp.toml
    echo "regular dependency was not removed"
    exit 1
}

echo "OK"
