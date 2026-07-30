#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 174_cache_modes_and_commands.sh — the three build modes, and the cache
# maintenance surface they need.
#
#   mcpp build                  read + write the global cache (default)
#   mcpp build --cache=local     neither; every dependency compiles in target/
#   mcpp build --cache=off       neither, and target/ is cleared first
#   mcpp build --no-cache        deprecated alias for --cache=off
#
# `--no-cache` used to be the only switch, and it only ever cleared target/ —
# a name that says nothing about a cache. It stays accepted.
#
# The maintenance half exists because a cache nobody can inspect, size or
# reclaim is a cache nobody can trust. `prune` in particular used to rank
# entries by the directory's mtime, which only records when an entry was
# WRITTEN — so a dependency that hit on every build looked as stale as one
# nobody had touched in a month. `gc` reads entry.json's `accessed` stamp.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
mkdir -p "$INDEX_DIR/pkgs/m"
cat > "$INDEX_DIR/pkgs/m/mode-lib.lua" <<'EOF'
package = {
    spec = "1",
    name = "mode-lib",
    description = "Cache mode fixture",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/mode-lib-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["mode-lib"] = { kind = "lib" } },
        deps = {},
    },
}
EOF

mkdir -p "$TMP/app/src" \
         "$TMP/app/.mcpp/.xlings/data/xpkgs/local-dev.mode-lib/1.0.0/src"
cat > "$TMP/app/.mcpp/.xlings/data/xpkgs/local-dev.mode-lib/1.0.0/src/lib.cppm" <<'EOF'
export module mode.lib;
export int mode_value() { return 5; }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
import std;
import mode.lib;
int main() { std::println("{}", mode_value()); return 0; }
EOF
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR" }

[dependencies]
"local-dev.mode-lib" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cd "$TMP/app"

entry_count() { find "$MCPP_HOME/build-cache/v1/pkg" -name entry.json 2>/dev/null | wc -l; }

# ── mcpp cache dir: where IS it? ────────────────────────────────────────────
out=$("$MCPP" cache dir 2>&1)
[[ "$out" == "$MCPP_HOME/build-cache/v1"* ]] || {
    echo "FAIL: cache dir reported '$out', expected '$MCPP_HOME/build-cache/v1'"
    exit 1
}

# ── local: nothing read, nothing written ───────────────────────────────────
"$MCPP" build --cache=local > local.log 2>&1 || { cat local.log; exit 1; }
[[ "$(entry_count)" -eq 0 ]] || {
    echo "FAIL: --cache=local wrote $(entry_count) cache entries"
    find "$MCPP_HOME/build-cache/v1/pkg" -maxdepth 4 2>/dev/null
    exit 1
}
# The dependency still has to be built, in-project.
NINJA="$(find target -name build.ninja | head -1)"
grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA" || {
    echo "FAIL: --cache=local did not compile the dependency locally"
    grep -n 'mode-lib' "$NINJA" | head
    exit 1
}
./target/*/*/bin/app > run.log 2>&1 || { cat run.log; exit 1; }
grep -q '^5$' run.log || { echo "FAIL: local build produced wrong output"; cat run.log; exit 1; }

# ── global (default): writes an entry ──────────────────────────────────────
rm -rf target
"$MCPP" build > global.log 2>&1 || { cat global.log; exit 1; }
[[ "$(entry_count)" -eq 1 ]] || {
    echo "FAIL: default build wrote $(entry_count) entries, expected 1"
    cat global.log
    exit 1
}

# ── cache list / list --json / info / verify ───────────────────────────────
"$MCPP" cache list > list.log 2>&1
grep -q 'mode-lib' list.log || { cat list.log; echo "FAIL: list omits the entry"; exit 1; }

"$MCPP" cache list --json > list.json 2>&1
python3 - list.json > jsoncheck.log 2>&1 <<'PYEOF' || { cat jsoncheck.log; exit 1; }
import json, sys
d = json.load(open(sys.argv[1]))
if "root" not in d or "entries" not in d:
    sys.exit(f"FAIL: list --json missing keys: {sorted(d)}")
pkgs = [e for e in d["entries"] if e["kind"] == "pkg"]
if not pkgs:
    sys.exit("FAIL: list --json has no package entries")
e = pkgs[0]
for k in ("kind", "label", "key", "dir", "bytes", "files", "accessed", "complete"):
    if k not in e:
        sys.exit(f"FAIL: entry missing '{k}': {sorted(e)}")
if not e["complete"]:
    sys.exit(f"FAIL: fresh entry reported incomplete: {e}")
if e["bytes"] <= 0:
    sys.exit(f"FAIL: entry reports {e['bytes']} bytes")
PYEOF

# info must print the recorded key inputs — that is the whole reason a cache
# entry describes itself, and the only way to diagnose a suspected wrong hit.
"$MCPP" cache info mode-lib > info.log 2>&1 || { cat info.log; exit 1; }
grep -q 'inputs' info.log || { cat info.log; echo "FAIL: info omits key inputs"; exit 1; }
grep -q 'opt_level' info.log || { cat info.log; echo "FAIL: info omits the profile axis"; exit 1; }

"$MCPP" cache verify > verify.log 2>&1 || { cat verify.log; echo "FAIL: verify failed on a healthy cache"; exit 1; }
grep -q 'all complete' verify.log || { cat verify.log; echo "FAIL: verify message"; exit 1; }

# verify must FAIL (non-zero) when an artifact is gone. Explicit if/exit rather
# than `! cmd`: under `set -e` a negated command is exempted from the errexit
# check, so `! "$MCPP" cache verify` can never fail this test.
victim="$(find "$MCPP_HOME/build-cache/v1/pkg" -path '*/obj/*' -type f | head -1)"
[[ -n "$victim" ]] || { echo "FAIL: no cached object to remove"; exit 1; }
mv "$victim" "$victim.bak"
rc=0
"$MCPP" cache verify > verify2.log 2>&1 || rc=$?
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: verify passed with a missing artifact"
    cat verify2.log
    exit 1
fi
grep -q 'incomplete' verify2.log || { cat verify2.log; echo "FAIL: verify wording"; exit 1; }
mv "$victim.bak" "$victim"

# ── off: clears this build's build dir and touches no cache ────────────────
# The build DIRECTORY (target/<triple>/<fp>/), not all of target/: sibling dirs
# for other profiles and other targets are not this invocation's business.
builddir="$(dirname "$(find target -name build.ninja | head -1)")"
marker="$builddir/_off_marker"
touch "$marker"
before="$(entry_count)"
"$MCPP" build --cache=off > off.log 2>&1 || { cat off.log; exit 1; }
[[ ! -f "$marker" ]] || { echo "FAIL: --cache=off did not clear the build dir"; exit 1; }
[[ "$(entry_count)" -eq "$before" ]] || {
    echo "FAIL: --cache=off changed the cache ($before -> $(entry_count))"
    exit 1
}
NINJA="$(find target -name build.ninja | head -1)"
grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA" || {
    echo "FAIL: --cache=off served the dependency from the cache"
    exit 1
}

# --no-cache must behave identically (deprecated alias).
touch "$marker"
"$MCPP" build --no-cache > nocache.log 2>&1 || { cat nocache.log; exit 1; }
[[ ! -f "$marker" ]] || { echo "FAIL: --no-cache did not clear the build dir"; exit 1; }

# An unknown mode is refused, not silently taken as "global".
rc=0
"$MCPP" build --cache=bogus --strict > bogus.log 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: --cache=bogus accepted under --strict"; cat bogus.log; exit 1; }
grep -qi 'cache mode' bogus.log || { cat bogus.log; echo "FAIL: unhelpful error"; exit 1; }

# MCPP_BUILD_CACHE is the env-level equivalent.
rm -rf target
MCPP_BUILD_CACHE=local "$MCPP" build > env.log 2>&1 || { cat env.log; exit 1; }
NINJA="$(find target -name build.ninja | head -1)"
grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA" || {
    echo "FAIL: MCPP_BUILD_CACHE=local was ignored"
    exit 1
}

# [build] cache is the project-level equivalent, and --cache beats it.
cat >> mcpp.toml <<'EOF'

[build]
cache = "local"
EOF
rm -rf target
"$MCPP" build > manifest.log 2>&1 || { cat manifest.log; exit 1; }
NINJA="$(find target -name build.ninja | head -1)"
grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA" || {
    echo "FAIL: [build] cache = local was ignored"
    exit 1
}
rm -rf target
"$MCPP" build --cache=global > override.log 2>&1 || { cat override.log; exit 1; }
NINJA="$(find target -name build.ninja | head -1)"
if grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA"; then
    echo "FAIL: --cache=global did not override [build] cache = local"
    exit 1
fi

# Drop the manifest override again — everything below assumes the default mode,
# and a leftover `cache = "local"` would make the gc section measure an empty
# cache and "pass" for the wrong reason.
python3 - mcpp.toml <<'PYEOF'
import sys
p = sys.argv[1]
lines = open(p).read().splitlines(keepends=True)
out, skip = [], False
for ln in lines:
    if ln.strip() == "[build]":
        skip = True
        continue
    if skip and ln.strip().startswith("cache"):
        skip = False
        continue
    out.append(ln)
open(p, "w").write("".join(out))
PYEOF
if grep -q '^cache' mcpp.toml; then
    echo "FAIL: could not strip the [build] cache override"
    cat mcpp.toml
    exit 1
fi

# ── the fast path must honour the declared mode ─────────────────────────────
# A build.ninja generated under `global` contains stage_file edges reading the
# cache. Replaying it for a request that asked for `local` would use the cache the
# manifest just said not to use — and ruling the cache out is `local`'s whole
# purpose. .build_cache therefore records the mode, exactly as it records the
# profile, and a mismatch is a miss.
cat >> mcpp.toml <<'EOF'

[build]
cache = "local"
EOF
rm -rf target
"$MCPP" build --cache=global > modeglobal.log 2>&1 || { cat modeglobal.log; exit 1; }
NINJA="$(find target -name build.ninja | head -1)"
staged_before=$(grep -c ': stage_file .*mode-lib' "$NINJA" || true)
[[ "$staged_before" -gt 0 ]] || {
    echo "FAIL: --cache=global did not produce stage edges (cache should be warm here)"
    cat modeglobal.log
    exit 1
}
# Bare build: the manifest says local, and the fast path must NOT replay the
# global graph. Note mcpp.toml is untouched between these two builds, so its
# mtime cannot be what saves us — only the recorded mode can.
"$MCPP" build > modebare.log 2>&1 || { cat modebare.log; exit 1; }
NINJA="$(find target -name build.ninja | head -1)"
staged_after=$(grep -c ': stage_file .*mode-lib' "$NINJA" || true)
[[ "$staged_after" -eq 0 ]] || {
    echo "FAIL: bare build replayed the global-mode graph ($staged_after stage edges)"
    echo "      [build] cache = local was bypassed by the fast path"
    cat modebare.log
    exit 1
}
grep -qE ': (cxx_module|cxx_object) .*mode-lib' "$NINJA" || {
    echo "FAIL: local mode did not compile the dependency"
    exit 1
}
# Strip the override again for the sections below.
python3 - mcpp.toml <<'PYEOF'
import sys
p = sys.argv[1]
out, skip = [], False
for ln in open(p).read().splitlines(keepends=True):
    if ln.strip() == "[build]":
        skip = True
        continue
    if skip and ln.strip().startswith("cache"):
        skip = False
        continue
    out.append(ln)
open(p, "w").write("".join(out))
PYEOF
rm -rf target
"$MCPP" build > moderestore.log 2>&1 || { cat moderestore.log; exit 1; }

# ── gc: LRU by last USE, not by when the entry was written ─────────────────
# Backdate the entry's accessed stamp, then confirm an age-bounded gc collects it
# — and that a build afterwards refreshes the stamp so it would survive next time.
entry="$(find "$MCPP_HOME/build-cache/v1/pkg" -name entry.json | head -1)"
python3 - "$entry" <<'PYEOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["accessed"] = "1000"          # 1970-ish
json.dump(d, open(p, "w"), indent=2)
PYEOF
"$MCPP" cache gc --older-than 1s > gc.log 2>&1 || { cat gc.log; exit 1; }
[[ "$(entry_count)" -eq 0 ]] || {
    echo "FAIL: gc did not collect a long-unused entry"
    cat gc.log
    exit 1
}
grep -q 'Collected' gc.log || { cat gc.log; echo "FAIL: gc wording"; exit 1; }

rm -rf target
"$MCPP" build > refill.log 2>&1 || { cat refill.log; exit 1; }
"$MCPP" build > touch.log 2>&1 || { cat touch.log; exit 1; }   # a HIT
"$MCPP" cache gc --older-than 1h > gc2.log 2>&1 || { cat gc2.log; exit 1; }
[[ "$(entry_count)" -eq 1 ]] || {
    echo "FAIL: gc collected an entry that was just used"
    cat gc2.log
    exit 1
}

# gc requires a budget; no arguments is a usage error, not a silent no-op.
rc=0
"$MCPP" cache gc > gcnoargs.log 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL: gc with no budget succeeded"; cat gcnoargs.log; exit 1; }

# gc --max-size evicts package entries to hit a budget, and leaves std entries
# alone: a std BMI is shared by every project on the machine and costs ~30 s to
# rebuild, so trading it for a little disk is the wrong trade. The summary must
# therefore talk about PACKAGE entries — reporting it as "cache now 0 B" would
# read as an empty cache while tens of MB of std BMIs sit next to it.
[[ "$(entry_count)" -eq 1 ]] || { echo "FAIL: expected one package entry before gc"; exit 1; }
[[ -d "$MCPP_HOME/build-cache/v1/std" ]] || { echo "FAIL: no std entries to protect"; exit 1; }
"$MCPP" cache gc --max-size 1B > gcsize.log 2>&1 || { cat gcsize.log; exit 1; }
[[ "$(entry_count)" -eq 0 ]] || { echo "FAIL: gc --max-size kept package entries"; cat gcsize.log; exit 1; }
[[ -d "$MCPP_HOME/build-cache/v1/std" ]] || {
    echo "FAIL: gc --max-size evicted std entries"
    cat gcsize.log
    exit 1
}
grep -q 'package entries now' gcsize.log || {
    cat gcsize.log
    echo "FAIL: gc summary must scope its figure to package entries"
    exit 1
}

# Refill for the clean tests below.
rm -rf target
"$MCPP" build > refill2.log 2>&1 || { cat refill2.log; exit 1; }

# ── clean --std / --all / --legacy ─────────────────────────────────────────
[[ -d "$MCPP_HOME/build-cache/v1/std" ]] || { echo "FAIL: no std cache dir"; exit 1; }
"$MCPP" cache clean --std > cleanstd.log 2>&1
[[ ! -d "$MCPP_HOME/build-cache/v1/std" ]] || {
    echo "FAIL: clean --std left the std entries"
    exit 1
}
[[ "$(entry_count)" -eq 1 ]] || { echo "FAIL: clean --std dropped package entries too"; exit 1; }

"$MCPP" cache clean --all > cleanall.log 2>&1
[[ "$(entry_count)" -eq 0 ]] || { echo "FAIL: clean --all left package entries"; exit 1; }

# --legacy targets the pre-v1 tree, which nothing reads any more.
mkdir -p "$MCPP_HOME/bmi/deadbeef/deps/idx/pkg@1.0.0"
echo x > "$MCPP_HOME/bmi/deadbeef/deps/idx/pkg@1.0.0/old.o"
"$MCPP" cache clean --legacy > cleanlegacy.log 2>&1
[[ ! -d "$MCPP_HOME/bmi" ]] || { cat cleanlegacy.log; echo "FAIL: clean --legacy left $MCPP_HOME/bmi"; exit 1; }
grep -q 'pre-v1' cleanlegacy.log || { cat cleanlegacy.log; echo "FAIL: legacy wording"; exit 1; }

echo "OK"
