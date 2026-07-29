#!/usr/bin/env bash
# requires:
# `mcpp add` modifies mcpp.toml [dependencies]. Default-namespace packages land
# as bare keys under [dependencies]; `ns:name` (non-default ns) uses a subtable;
# dotted selectors keep their spelling. A package no readable index can serve is
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

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# A local index carrying three packages: two in the DEFAULT namespace (so bare
# and `mcpplibs:`-qualified adds resolve against it) and one under `acme`.
# Descriptors are filed under the first letter of their FILENAME, and publish
# every OS so the version assertions below hold on all three CI platforms.
mkdir -p index/pkgs/w index/pkgs/g index/pkgs/a
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
cat >> mcpp.toml <<'EOF'

[indices]
default = { path = "index" }
acme = { path = "index" }
EOF

# (1) Default-namespace dep: bare name → unquoted key under [dependencies].
"$MCPP" add widget@0.1.0 > /dev/null
grep -qE '^\[dependencies\]'    mcpp.toml || { cat mcpp.toml; echo "no [dependencies] section"; exit 1; }
grep -qE '^widget = "0\.1\.0"$' mcpp.toml || { cat mcpp.toml; echo "widget entry missing or quoted"; exit 1; }
grep -qE '^"widget"'            mcpp.toml && { cat mcpp.toml; echo "default-ns key should not be quoted"; exit 1; }

# (2) `<ns>:<name>` where ns IS the default (mcpplibs) — still a bare key under
# [dependencies], NOT [dependencies.mcpplibs]. Appends without duplicating the
# section header.
"$MCPP" add mcpplibs:gadget@0.2.0 > /dev/null
header_count=$(grep -cE '^\[dependencies\]$' mcpp.toml)
[[ "$header_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies] header duplicated"; exit 1; }
grep -qE '^gadget = "0\.2\.0"$' mcpp.toml || { cat mcpp.toml; echo "gadget not set"; exit 1; }

# (3) Dotted selector input is preserved under the single [dependencies] table.
# `acme.util` is a NAMESPACE PATH, not a name: it resolves as (acme, util). A
# gate probing the literal short name "acme.util" would refuse this — no
# package.name ever contains a dot (SPEC-001 §3.2).
"$MCPP" add acme.util@2.0.0 > /dev/null
grep -qE '^acme\.util = "2\.0\.0"$' mcpp.toml || { cat mcpp.toml; echo "acme.util selector entry missing"; exit 1; }

# (4) Colon form remains explicit namespace syntax and uses a subtable.
"$MCPP" add acme:util@2.0.0 > /dev/null
grep -qE '^\[dependencies\.acme\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.acme] section"; exit 1; }
grep -qE '^util = "2\.0\.0"$'       mcpp.toml || { cat mcpp.toml; echo "util entry missing"; exit 1; }

# (5) Dotted remove can still clean the old subtable shape for compatibility.
cat >> mcpp.toml <<'EOF'

[dependencies.legacy]
old = "0.1.0"
EOF
"$MCPP" remove legacy.old > /dev/null
! grep -qE '^old = "0\.1\.0"$' mcpp.toml || { cat mcpp.toml; echo "legacy.old was not removed"; exit 1; }

# (6) Reject missing version.
err=$("$MCPP" add bareword 2>&1) && { echo "expected error for missing version"; exit 1; }
[[ "$err" == *"version required"* ]] || { echo "wrong error: $err"; exit 1; }

# (7) Reject empty package name (e.g. `mcpp add :foo@1.0`).
err=$("$MCPP" add ":@1.0" 2>&1) && { echo "expected error for empty package name"; exit 1; }

# (8) A package the index does not carry is refused, and mcpp.toml is left
# exactly as it was. The error names the identities that were tried.
cp mcpp.toml "$TMP/before"
err=$("$MCPP" add definitely-not-a-real-package@9.9.9 2>&1) && { echo "expected error for missing package"; exit 1; }
[[ "$err" == *"not found"* ]] || { echo "wrong error: $err"; exit 1; }
[[ "$err" == *"tried:"*    ]] || { echo "error should list the identities tried: $err"; exit 1; }
diff -q "$TMP/before" mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for missing package"; exit 1; }

# (9) Same for an explicitly-namespaced miss in a readable index.
err=$("$MCPP" add acme:nope@1.0.0 2>&1) && { echo "expected error for missing acme package"; exit 1; }
[[ "$err" == *"(acme, nope)"* ]] || { echo "wrong error: $err"; exit 1; }
diff -q "$TMP/before" mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for missing package"; exit 1; }

# (10) A namespace no readable index covers cannot be refuted, so the add goes
# through unverified rather than failing. Refusing here would be worse than the
# bug being fixed — `mcpp build` resolves such dependencies fine.
"$MCPP" add unknownidx:thing@1.0.0 > /dev/null
grep -qE '^\[dependencies\.unknownidx\]$' mcpp.toml || { cat mcpp.toml; echo "unverifiable namespace should still be added"; exit 1; }

# (11) A real package asked for at a version it does not publish is a warning,
# not a refusal — version tables are per-OS, so "absent here" is not "absent".
out=$("$MCPP" add widget@9.9.9 2>&1)
[[ "$out" == *"available: 0.1.0"* ]] || { echo "expected available-versions warning: $out"; exit 1; }
grep -qE '^widget = "9\.9\.9"$' mcpp.toml || { cat mcpp.toml; echo "widget@9.9.9 should still be written"; exit 1; }

# (12) A workspace MEMBER inherits the root's [indices], so adding inside it
# sees the same packages `mcpp build` would resolve from there (#224).
mkdir -p "$TMP/ws"
cd "$TMP/ws"
cp -r "$TMP/myapp/index" index
cat > mcpp.toml <<'EOF'
[workspace]
members = ["m1"]

[indices]
acme = { path = "index" }
EOF
"$MCPP" new m1 > /dev/null
cd m1
"$MCPP" add acme:util@2.0.0 > /dev/null
grep -qE '^util = "2\.0\.0"$' mcpp.toml || { cat mcpp.toml; echo "member should inherit workspace [indices]"; exit 1; }
err=$("$MCPP" add acme:nope@1.0.0 2>&1) && { echo "expected error inside workspace member"; exit 1; }
[[ "$err" == *"(acme, nope)"* ]] || { echo "wrong error: $err"; exit 1; }

echo "OK"
