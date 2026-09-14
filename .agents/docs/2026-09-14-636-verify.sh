#!/usr/bin/env bash
# Ecosystem verification for the design record
# 2026-09-14-636-build-database-and-the-latest-xlings.md, against a PUBLISHED
# mcpp and the xlings it pins.
#
#   B64=$(base64 -w0 .agents/docs/2026-09-14-636-verify.sh)
#   xlings subos new verify-636        # once
#   xlings subos use verify-636 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=<mcpp> XLINGS_VERIFY_VERSION=<xlings> bash /tmp/v.sh"
#
# The sandbox starts from an empty $HOME and a fresh /tmp and shares the xlings
# data directory, so the published mcpp is addressed by its store path. The mcpp
# home is removed first: every section reads this release, not a previous run.
# A section that cannot run says so and is listed again in the summary.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
XVER="${XLINGS_VERIFY_VERSION:?set XLINGS_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }

root=/tmp/verify-636
rm -rf "$root" "$HOME/.mcpp"; mkdir -p "$root"
REG="$HOME/.mcpp/registry"

section "A. the published mcpp, the CN mirror, and the xlings it vendors"
if [ ! -x "$STORE" ]; then
    fail "no mcpp at $STORE"; printf '\nfails=%d (nothing else can run)\n' "$fails"; exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
case "$got" in *"$VER"*) ok "mcpp --version says $got" ;; *) fail "mcpp --version says '$got', expected $VER" ;; esac
"$STORE" self config --mirror CN > "$root/mirror.log" 2>&1 && ok "mcpp self config --mirror CN" || { fail "mcpp self config --mirror CN"; tail -3 "$root/mirror.log"; }
grep -q '"mirror": "CN"' "$REG/.xlings.json" 2>/dev/null && ok "the registry's xlings configuration reads mirror CN" || fail "the registry's .xlings.json does not read mirror CN"
"$STORE" self env > "$root/env.log" 2>&1
grep -q "xlings pinned       = $XVER" "$root/env.log" && ok "the pin is xlings $XVER" || { fail "mcpp self env does not pin xlings $XVER"; grep 'xlings pinned' "$root/env.log"; }
vend=$("$REG/bin/xlings" --version 2>/dev/null | head -1)
case "$vend" in *"$XVER"*) ok "the vendored xlings is $vend" ;; *) fail "the vendored xlings is '$vend', expected $XVER" ;; esac

section "B. a hookless package after a hook package: its own archive, nothing else"
d=$root/b; rm -rf "$d"; mkdir -p "$d/src"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name = "eco636"
version = "0.1.0"
standard = "c++23"

[dependencies]
mcpplibs.cmdline = "0.0.1"
EOF
printf 'import std;\nimport mcpplibs.cmdline;\nint main() { std::println("1-2-3"); }\n' > "$d/src/main.cpp"
if (cd "$d" && "$STORE" build > build.log 2>&1); then
    ok "a project with a toolchain install and a hookless mcpp-index dependency builds"
    swept=$(find "$REG/data/xpkgs" -mindepth 3 -maxdepth 3 -name '*.lock' 2>/dev/null)
    [ -z "$swept" ] && ok "no store payload carries a download sidecar" || { fail "store payloads carry download sidecars"; printf '%s\n' "$swept" | head -5; }
    cmd=$(find "$REG/data/xpkgs" -maxdepth 2 -path '*cmdline*' -type d | tail -1)
    if [ -n "$cmd" ]; then
        top=$(ls -A "$cmd" | grep -v '^\.' | tr '\n' ' ')
        case "$top" in
            *.tar.gz*|*.zip*) fail "the cmdline payload holds an archive: $top" ;;
            *) ok "the cmdline payload's top level is its own archive's entries: $top" ;;
        esac
        # cmdline's index entry describes 0.0.1 inline, with globs such as
        # `*/src/**/*.cppm` whose `*/` is the archive's own top-level directory.
        tops=$(find "$cmd" -mindepth 1 -maxdepth 1 -type d ! -name '.*' | wc -l)
        if [ "$tops" -eq 1 ] && [ -n "$(find "$cmd" -mindepth 3 -maxdepth 3 -path '*/src/*.cppm')" ]; then
            ok "the archive's top-level directory is kept, so the entry's */src globs match"
        else
            fail "the payload does not have one top-level directory holding src/*.cppm"; ls -A "$cmd"
        fi
    else
        skip "B: the cmdline payload directory was not found"
    fi
    (cd "$d" && "$STORE" run > run.log 2>&1); grep -qx '1-2-3' "$d/run.log" && ok "mcpp run prints 1-2-3" || { fail "mcpp run"; tail -3 "$d/run.log"; }
else
    fail "the dependency build failed"; tail -8 "$d/build.log"
fi

section "C. mcpp emit build-database writes nothing and describes the plan"
d=$root/c; rm -rf "$d"; mkdir -p "$d/src" "$d/tests"
cat > "$d/mcpp.toml" <<'EOF'
[package]
name = "hello"
version = "0.1.0"
standard = "c++23"
EOF
printf 'export module hello.greet;\nexport import :detail;\nimport std;\nexport std::string greet();\n' > "$d/src/greet.cppm"
printf 'export module hello.greet:detail;\nexport int answer() { return 42; }\n' > "$d/src/detail.cppm"
printf 'module hello.greet;\nstd::string greet() { return "hi"; }\n' > "$d/src/greet_impl.cpp"
printf 'import hello.greet;\nimport std;\nint main() { std::println("{}", greet()); }\n' > "$d/src/main.cpp"
printf 'int main() { return 0; }\n' > "$d/tests/test_smoke.cpp"
before=$(cd "$d" && find . -type f | sort | xargs sha256sum | sha256sum)
if ! command -v python3 >/dev/null 2>&1; then
    skip "C: no python3 in the sandbox to read the document"
elif (cd "$d" && "$STORE" emit build-database --format json > "$root/c.json" 2> "$root/c.err"); then
    after=$(cd "$d" && find . -type f | sort | xargs sha256sum | sha256sum)
    [ "$before" = "$after" ] && ok "the project tree is unchanged" || fail "the project tree changed"
    if python3 - "$root/c.json" <<'EOF'
import json, os, sys
e = json.load(open(sys.argv[1]))
assert e["kind"] == "mcpp.build-database" and "write-project" not in e["effects"]
db = e["data"]["database"]
sets = {s["name"]: s for s in db["sets"]}
assert set(sets) == {"hello", "hello:test", "mcpp:std"}, sorted(sets)
roles = {os.path.basename(u["source"]): u["ide"]["role"] for u in sets["hello"]["translation-units"]}
assert roles == {"greet.cppm": "module-interface", "detail.cppm": "module-partition-interface",
                 "greet_impl.cpp": "module-implementation", "main.cpp": "non-module"}, roles
std = sets["mcpp:std"]["translation-units"][0]
assert std["provides"] == {"std": ""} and os.path.isfile(std["source"]), std
print("sets", sorted(sets), "units", sum(len(s["translation-units"]) for s in db["sets"]))
EOF
    then ok "the S1 document has the sets, roles and std unit SPEC-005 states"
    else fail "the S1 document"; fi
else
    fail "emit build-database failed"; tail -5 "$root/c.err"
fi

section "C2. the build database of a project with an mcpp-index dependency"
d=$root/b
if [ ! -f "$d/mcpp.toml" ] || ! command -v python3 >/dev/null 2>&1; then
    skip "C2: section B's project or python3 is missing"
else
    before=$(cd "$d" && find . -type f | sort | xargs sha256sum | sha256sum)
    if (cd "$d" && "$STORE" emit build-database --format json > "$root/c2.json" 2> "$root/c2.err"); then
        after=$(cd "$d" && find . -type f | sort | xargs sha256sum | sha256sum)
        [ "$before" = "$after" ] && ok "the built project's tree is unchanged, target/ included" || fail "the built project's tree changed"
        if python3 - "$root/c2.json" "$REG" <<'EOF'
import json, os, sys
e = json.load(open(sys.argv[1])); reg = os.path.realpath(sys.argv[2])
db, watch = e["data"]["database"], e["data"]["watch"]
sets = {s["name"]: s for s in db["sets"]}
assert {"eco636", "mcpplibs.cmdline", "mcpp:std"} <= set(sets), sorted(sets)
assert all(not os.path.realpath(w).startswith(reg) for w in watch if os.path.isabs(w)), watch
for tid, t in db["ide"]["toolchains"].items():
    assert all(os.path.isfile(f) for f in t["config-files"]), (tid, t["config-files"])
for s in db["sets"]:
    for u in s["translation-units"]:
        head = [u["arguments"][0]] + s["baseline-arguments"] + u["local-arguments"]
        assert u["arguments"][:len(head)] == head and u["private"] is False, (s["name"], u["source"])
print("sets", sorted(sets), "watch", len(watch))
EOF
        then ok "the document has the dependency's set, watches no store path, and carries the S1 SHOULD fields"
        else fail "the dependency project's document"; fi
    else
        fail "emit build-database failed on the dependency project"; tail -5 "$root/c2.err"
    fi
fi

section "D. xlings reports and repairs a payload that holds another package's download"
cmd=$(find "$REG/data/xpkgs" -maxdepth 2 -path '*cmdline*' -type d | tail -1)
strip_ansi() { sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; }
if [ -n "$cmd" ]; then
    # The control: the store section B produced has no finding of this kind.
    # The sandbox shell exports XLINGS_ACTIVE_SUBOS for its own home; mcpp's
    # xlings calls drop it (2026.9.14.3), and so do these.
    XLINGS_HOME="$REG" env -u XLINGS_ACTIVE_SUBOS "$REG/bin/xlings" self doctor 2>&1 | strip_ansi > "$root/doctor0.log"
    grep -qi 'swept payload' "$root/doctor0.log" && { fail "self doctor reports a swept payload before any was seeded"; grep -i -A2 'swept' "$root/doctor0.log" | head -6; } || ok "the fresh store has no swept-payload finding"
    # The downloader's shape: an archive beside its zero-length lock.
    printf 'x' > "$cmd/intruder-1.0-linux-x86_64.tar.gz"; : > "$cmd/intruder-1.0-linux-x86_64.tar.gz.lock"
    XLINGS_HOME="$REG" env -u XLINGS_ACTIVE_SUBOS "$REG/bin/xlings" self doctor 2>&1 | strip_ansi > "$root/doctor.log"
    if grep -qi 'swept payload' "$root/doctor.log" && grep -qF "intruder-1.0-linux-x86_64.tar.gz.lock" "$root/doctor.log"; then
        ok "self doctor reports the swept payload and names the lock file"
    else
        fail "self doctor did not report the seeded payload"; tail -12 "$root/doctor.log"
    fi
    seeded_at=$(date +%s)
    sleep 1
    XLINGS_HOME="$REG" env -u XLINGS_ACTIVE_SUBOS "$REG/bin/xlings" self doctor --fix 2>&1 | strip_ansi > "$root/fix.log"
    cmd=$(find "$REG/data/xpkgs" -maxdepth 2 -path '*cmdline*' -type d | tail -1)
    stamped=$(stat -c %Y "$cmd/.xpkg-install.json" 2>/dev/null || echo 0)
    if [ -n "$cmd" ] && [ ! -e "$cmd/intruder-1.0-linux-x86_64.tar.gz.lock" ] && [ ! -e "$cmd/intruder-1.0-linux-x86_64.tar.gz" ] \
       && [ "$stamped" -gt "$seeded_at" ] && [ -n "$(find "$cmd" -mindepth 3 -maxdepth 3 -path '*/src/*.cppm')" ]; then
        ok "self doctor --fix reinstalls the payload: no seeded file, a new install record, and */src/*.cppm is back"
    else
        fail "the payload after --fix"; ls -A "$cmd" 2>/dev/null | head; tail -12 "$root/fix.log"
    fi
else
    skip "D: section B produced no cmdline payload to seed"
fi

section "E. Windows: the probe reaches no shell"
skip "E: a Linux sandbox cannot run cmd.exe; e2e 687 and XlingsVersionPin.ProbeReadsStandardOutputThroughTheLauncher are the Windows criteria (CI)"

printf '\n== summary ==\nfails=%d\n' "$fails"
[ -n "$skipped" ] && printf 'not run:%s\n' "$skipped"
[ "$fails" -eq 0 ]
