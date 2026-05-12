#!/usr/bin/env bash
# 23_remove_update.sh — M4 #3: mcpp remove / mcpp update.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"

cd "$TMP"
"$MCPP" new myapp >/dev/null
cd myapp

"$MCPP" add foo@1.0.0 >/dev/null
"$MCPP" add bar@2.0.0 >/dev/null
# Default-namespace deps land as unquoted bare keys after the namespace
# refactor; accept either form so this stays robust across versions.
grep -qE '^("foo"|foo) = "1\.0\.0"' mcpp.toml || { cat mcpp.toml; echo "foo not added"; exit 1; }
grep -qE '^("bar"|bar) = "2\.0\.0"' mcpp.toml || { cat mcpp.toml; echo "bar not added"; exit 1; }

# remove foo
"$MCPP" remove foo > /tmp/_r.log 2>&1
grep -q 'Removing'        /tmp/_r.log || { cat /tmp/_r.log; exit 1; }
if grep -qE '^("foo"|foo) = ' mcpp.toml; then echo "foo not actually removed"; cat mcpp.toml; exit 1; fi
grep -qE '^("bar"|bar) = "2\.0\.0"' mcpp.toml || { echo "bar accidentally removed"; cat mcpp.toml; exit 1; }

# remove non-existent → exit code 1
rc=0
"$MCPP" remove nonexistent > /tmp/_r2.log 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || { echo "remove nonexistent should exit 1, got $rc"; exit 1; }

# Set up a fake mcpp.lock to test update
cat > mcpp.lock <<'EOF'
version = 1
[package.bar]
version = "2.0.0"
source  = "mcpplibs+https://example.com"
hash    = "sha256:placeholder"
EOF

# Targeted update bar
"$MCPP" update bar > /tmp/_u.log 2>&1
grep -q 'Updating' /tmp/_u.log || { cat /tmp/_u.log; exit 1; }
if grep -q 'bar' mcpp.lock; then
    echo "update bar didn't drop bar from lock"; cat mcpp.lock; exit 1
fi

# Wholesale update (no arg) → wipes lock
echo 'version = 1' > mcpp.lock   # ensure file exists
"$MCPP" update > /tmp/_u2.log 2>&1
grep -q 'all dependencies' /tmp/_u2.log || { cat /tmp/_u2.log; exit 1; }
[[ ! -f mcpp.lock ]] || { echo "lock not removed"; exit 1; }

echo "OK"
