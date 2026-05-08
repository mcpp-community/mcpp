#!/usr/bin/env bash
# `mcpp add` modifies mcpp.toml [dependencies], including the namespaced form
# `mcpp add <ns>:<name>@<ver>` which lands under [dependencies.<ns>] without
# any TOML key quoting.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp

# (1) Default-namespace dep: bare name → unquoted key under [dependencies].
"$MCPP" add somedep@0.1.0 > /dev/null
grep -qE '^\[dependencies\]'        mcpp.toml || { cat mcpp.toml; echo "no [dependencies] section"; exit 1; }
grep -qE '^somedep = "0\.1\.0"$'    mcpp.toml || { cat mcpp.toml; echo "somedep entry missing or quoted"; exit 1; }
grep -qE '^"somedep"'               mcpp.toml && { cat mcpp.toml; echo "default-ns key should not be quoted"; exit 1; }

# (2) Second default-ns dep — append, do not duplicate the section header.
"$MCPP" add another@0.2.0 > /dev/null
header_count=$(grep -cE '^\[dependencies\]$' mcpp.toml)
[[ "$header_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies] header duplicated"; exit 1; }
grep -qE '^another = "0\.2\.0"$' mcpp.toml || { cat mcpp.toml; echo "another not set"; exit 1; }

# (3) Namespaced dep via `<ns>:<name>@<ver>` lands in [dependencies.<ns>].
"$MCPP" add mcpplibs:cmdline@0.0.2 > /dev/null
grep -qE '^\[dependencies\.mcpplibs\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.mcpplibs] section"; exit 1; }
grep -qE '^cmdline = "0\.0\.2"$'        mcpp.toml || { cat mcpp.toml; echo "cmdline entry missing"; exit 1; }

# (4) A second package in the same namespace — appends under the existing subtable.
"$MCPP" add mcpplibs:templates@0.0.1 > /dev/null
ns_count=$(grep -cE '^\[dependencies\.mcpplibs\]$' mcpp.toml)
[[ "$ns_count" == "1" ]] || { cat mcpp.toml; echo "[dependencies.mcpplibs] header duplicated"; exit 1; }
grep -qE '^templates = "0\.0\.1"$' mcpp.toml || { cat mcpp.toml; echo "templates entry missing"; exit 1; }

# (5) Legacy dotted form is still accepted on input — written out as namespaced subtable.
"$MCPP" add acme.util@2.0.0 > /dev/null
grep -qE '^\[dependencies\.acme\]$' mcpp.toml || { cat mcpp.toml; echo "missing [dependencies.acme] section"; exit 1; }
grep -qE '^util = "2\.0\.0"$'      mcpp.toml || { cat mcpp.toml; echo "util entry missing"; exit 1; }

# (6) Reject missing version.
err=$("$MCPP" add bareword 2>&1) && { echo "expected error for missing version"; exit 1; }
[[ "$err" == *"version required"* ]] || { echo "wrong error: $err"; exit 1; }

# (7) Reject empty package name (e.g. `mcpp add :foo@1.0`).
err=$("$MCPP" add ":@1.0" 2>&1) && { echo "expected error for empty package name"; exit 1; }

echo "OK"
