#!/usr/bin/env bash
# 22_doctor_cache_publish.sh — M4 #1 #2 #4 smoke tests:
#   - mcpp doctor    runs and reports
#   - mcpp cache list / clean
#   - mcpp publish --dry-run
#   - mcpp --explain E0001
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME="$TMP/mcpp-home"

# 1) doctor: should always run, exit 0 or 1 (warn ok), never 2
rc=0
"$MCPP" self doctor > doctor.log 2>&1 || rc=$?
[[ "$rc" -le 1 ]] || { echo "doctor exit code $rc unexpected"; cat doctor.log; exit 1; }
grep -q 'Checking toolchain'      doctor.log || { cat doctor.log; echo "no toolchain check"; exit 1; }
grep -q 'Checking std module'     doctor.log || { cat doctor.log; echo "no std check"; exit 1; }
grep -q 'Checking registry'       doctor.log || { cat doctor.log; echo "no registry check"; exit 1; }
grep -q 'Checking cache health'   doctor.log || { cat doctor.log; echo "no cache check"; exit 1; }
grep -q 'Doctor result'             doctor.log || { cat doctor.log; echo "no result line"; exit 1; }

# 2) cache list (empty): friendly message
out=$("$MCPP" cache list 2>&1)
[[ "$out" == *"empty"* ]] || { echo "cache list empty: '$out'"; exit 1; }

# 3) publish dry-run on a fresh package. Publish uses `git archive` for the
#    source tarball, so we git-init + commit first. We also need a non-empty
#    [package].repo for the convention-based tarball URL — patch the
#    auto-generated mcpp.toml in place.
cd "$TMP"
"$MCPP" new myapp >/dev/null
cd myapp
sed -i 's|^repo[[:space:]]*=.*|repo = "https://github.com/example/myapp"|' mcpp.toml
grep -q '^repo' mcpp.toml || cat >> mcpp.toml <<'EOF'

[package]
repo = "https://github.com/example/myapp"
EOF

git init -q
git config user.email "test@local"
git config user.name  "test"
git add .
git commit -q -m "init"

out=$("$MCPP" publish --dry-run 2>&1)
[[ "$out" == *"Packaging myapp"*       ]] || { echo "no Packaging line: $out"; exit 1; }
[[ "$out" == *"Tarball"*               ]] || { echo "no Tarball status: $out"; exit 1; }
[[ "$out" == *"SHA-256"*               ]] || { echo "no SHA-256 status: $out"; exit 1; }
[[ "$out" == *"--- xpkg.lua content ---"* ]] || { echo "missing dry-run header: $out"; exit 1; }
[[ "$out" == *"name = \"myapp\""*      ]] || { echo "missing pkg name in lua: $out"; exit 1; }
[[ "$out" == *"--- end ---"*           ]] || { echo "missing end marker: $out"; exit 1; }
[[ "$out" == *"Next steps"*            ]] || { echo "missing next-steps section: $out"; exit 1; }
[[ -f target/dist/myapp-0.1.0.tar.gz ]] || { echo "tarball not created"; exit 1; }
[[ -f target/dist/myapp.lua ]]           || { echo "xpkg.lua not created"; exit 1; }

# 4) cache prune missing flag
rc=0
"$MCPP" cache prune > /tmp/_p.log 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || { echo "prune w/o --older-than should fail"; exit 1; }
grep -q 'older-than' /tmp/_p.log || { cat /tmp/_p.log; echo "no helpful error"; exit 1; }

# 5) cache clean (idempotent, even on empty)
"$MCPP" cache clean > /tmp/_c.log 2>&1
grep -q 'Cleaned' /tmp/_c.log || { cat /tmp/_c.log; echo "no clean message"; exit 1; }

# 6) --explain
out=$("$MCPP" --explain E0001 2>&1)
[[ "$out" == *"E0001"* && "$out" == *"name mismatch"* ]] || {
    echo "--explain output: $out"; exit 1; }
rc=0
"$MCPP" --explain E9999 > /tmp/_x.log 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || { echo "unknown code should fail"; exit 1; }

echo "OK"
