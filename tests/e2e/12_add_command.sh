#!/usr/bin/env bash
# requires:
# `mcpp add` modifies mcpp.toml [dependencies]. Default-namespace packages land
# as bare keys under [dependencies]; `ns:name` (non-default ns) uses a subtable.
# Non-existent packages are rejected before mcpp.toml is mutated.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# (1) Default-namespace dep via `<ns>:<name>@<ver>` where ns is the default
# (mcpplibs). Since 0.0.10+ default namespace is "mcpplibs", this lands as a
# bare key under [dependencies], NOT in [dependencies.mcpplibs].
"$MCPP" add mcpplibs:cmdline@0.0.1 > /dev/null
grep -qE '^\[dependencies\]'        mcpp.toml || { cat mcpp.toml; echo "no [dependencies] section"; exit 1; }
grep -qE '^cmdline = "0\.0\.1"$'    mcpp.toml || { cat mcpp.toml; echo "cmdline entry missing or wrong version"; exit 1; }
grep -qE '^"cmdline"'               mcpp.toml && { cat mcpp.toml; echo "default-ns key should not be quoted"; exit 1; }

# (2) Second default-ns dep — append, do not duplicate the section header.
"$MCPP" add mcpplibs:templates@0.0.1 > /dev/null
header_count=$(grep -cE '^\[dependencies\]$' mcpp.toml)
[[ "$header_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies] header duplicated"; exit 1; }
grep -qE '^templates = "0\.0\.1"$' mcpp.toml || { cat mcpp.toml; echo "templates not set"; exit 1; }

# (3) Colon form remains explicit namespace syntax and uses a subtable.
"$MCPP" add compat:gtest@1.15.2 > /dev/null
grep -qE '^\[dependencies\.compat\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.compat] section"; exit 1; }
grep -qE '^gtest = "1\.15\.2"$'        mcpp.toml || { cat mcpp.toml; echo "gtest entry missing"; exit 1; }

# (4) Dotted remove can still clean the old subtable shape for compatibility.
cat >> mcpp.toml <<'EOF'

[dependencies.legacy]
old = "0.1.0"
EOF
"$MCPP" remove legacy.old > /dev/null
! grep -qE '^old = "0\.1\.0"$' mcpp.toml || { cat mcpp.toml; echo "legacy.old was not removed"; exit 1; }

# (5) Reject missing version.
err=$("$MCPP" add bareword 2>&1) && { echo "expected error for missing version"; exit 1; }
[[ "$err" == *"version required"* ]] || { echo "wrong error: $err"; exit 1; }

# (6) Reject empty package name (e.g. `mcpp add :foo@1.0`).
err=$("$MCPP" add ":@1.0" 2>&1) && { echo "expected error for empty package name"; exit 1; }

# (7) Reject non-existent package and do not mutate mcpp.toml.
cp mcpp.toml mcpp.toml.before
err=$("$MCPP" add definitely-not-a-real-package@9.9.9 2>&1) && { echo "expected error for missing package"; exit 1; }
[[ "$err" == *"not found"* ]] || { echo "wrong error: $err"; exit 1; }
diff -q mcpp.toml.before mcpp.toml || { cat mcpp.toml; echo "mcpp.toml mutated for missing package"; exit 1; }

echo "OK"
