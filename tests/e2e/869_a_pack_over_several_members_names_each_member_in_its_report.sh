#!/usr/bin/env bash
# requires: unix-shell python3
# 869 -- what `mcpp pack` over several members writes and reports for the built-in
# formats: each member's archive or tree where it would be written alone, below a
# directory `--output` names, and each member named in the report.
#
# mcpp#749 (K1, member selection design 2026-09-30).
#
# Criteria:
#   L. `--workspace` and a repeated `-p` pack the same members, in `[workspace]
#      members` order, each into its own `target/dist` under the name it has
#      when packed alone; an archive is the one `mcpp pack -p <member>` makes.
#      Without a selector a virtual workspace root packs its first member with a
#      program, as it always did.
#   M. `--output <dir>` writes each member's archive, or with `--format dir`
#      its tree, below the directory, under its default name.
#   N. `--exclude` removes a member from `--workspace`.
#   P. Members of two configurations (one is `standard = "c++26"`) are planned in
#      two groups, each built once, and every member is packed from its own.
#   O. `--message-format json` keeps the envelope: `data.artifacts` lists every
#      member's artifacts, each naming its member, `data.stage` stays null for
#      several members, and `data.stages` names one tree per member. For one
#      member the envelope is the one it always was: no `member`, no `stages`.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["core", "gui", "cli"]
EOF
mkdir -p core/src
cat > core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
EOF
printf 'export module zcore;\nexport int core_answer() { return 42; }\n' > core/src/core.cppm
for m in cli gui; do
    mkdir -p $m/src
    cat > $m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
    printf '#include <cstdio>\nimport zcore;\nint main() { std::printf("%s %%d\\n", core_answer()); return 0; }\n' "$m" > $m/src/main.cpp
done

archive_of() { find "$1/target/dist" -maxdepth 1 -name "$1-0.1.0-*.tar.gz" 2>/dev/null | head -1; }
members_in() { python3 -c 'import json,sys; d=json.load(sys.stdin)["data"]; print(" ".join(a.get("member","-") for a in d["artifacts"]))'; }

# ── L ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --mode system --workspace > l1.log 2>&1 || fail "L: --workspace did not pack" l1.log
for m in cli gui; do
    [ -n "$(archive_of $m)" ] || fail "L: $m was not packed below its own target/dist" l1.log
done
[ -z "$(find core -name '*.tar.gz' | head -1)" ] || fail "L: the library was packed as a program" l1.log
# Order: [workspace] members lists gui before cli, and the report follows it.
gui_at=$(grep -n "Packing gui" l1.log | head -1 | cut -d: -f1)
cli_at=$(grep -n "Packing cli" l1.log | head -1 | cut -d: -f1)
[ -n "$gui_at" ] && [ -n "$cli_at" ] && [ "$gui_at" -lt "$cli_at" ] \
    || fail "L: the members are not reported in [workspace] members order" l1.log
mkdir together
for m in cli gui; do cp "$(archive_of $m)" together/$m.tar.gz; done
rm -rf cli/target/dist gui/target/dist
# A repeated -p selects the same members, whatever order it is written in.
"$MCPP" pack --mode system -p cli -p gui > l2.log 2>&1 || fail "L: -p cli -p gui did not pack" l2.log
for m in cli gui; do
    [ -n "$(archive_of $m)" ] || fail "L: -p cli -p gui did not pack $m" l2.log
done
# The archive is the one a member packed alone makes.
mkdir alone
for m in cli gui; do
    rm -rf $m/target/dist
    "$MCPP" pack --mode system -p $m > l3-$m.log 2>&1 || fail "L: mcpp pack -p $m failed" l3-$m.log
    cp "$(archive_of $m)" alone/$m.tar.gz
    mkdir -p together/x-$m alone/x-$m
    tar -xzf together/$m.tar.gz -C together/x-$m
    tar -xzf alone/$m.tar.gz -C alone/x-$m
    diff -r together/x-$m alone/x-$m > l4-$m.diff || fail "L: $m's archive from the pack of several differs from the one packed alone" l4-$m.diff
done
# Without a selector a virtual workspace root packs the first member that has a
# program, as it always did; it is --workspace that packs them all.
rm -rf cli/target/dist gui/target/dist
"$MCPP" pack --mode system > l5.log 2>&1 || fail "L: a bare mcpp pack at the workspace root failed" l5.log
[ -n "$(archive_of gui)" ] && [ -z "$(archive_of cli)" ] \
    || fail "L: a bare mcpp pack at the workspace root did not pack the first member with a program alone" l5.log
echo "ok: L, --workspace and a repeated -p pack each member as it is packed alone, in member order"

# ── M ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --mode system --workspace -o out > m1.log 2>&1 || fail "M: --output <dir> did not pack" m1.log
for m in cli gui; do
    [ -n "$(find out -maxdepth 1 -name "$m-0.1.0-*.tar.gz" | head -1)" ] \
        || fail "M: $m's archive is not below the directory --output names" m1.log
done
[ "$(find out -maxdepth 1 -name '*.tar.gz' | wc -l | tr -d ' ')" = 2 ] || fail "M: the directory holds more than the two archives" m1.log
"$MCPP" pack --mode system --workspace --format dir -o out-trees > m2.log 2>&1 || fail "M: --format dir with --output <dir> did not pack" m2.log
for m in cli gui; do
    t=$(find out-trees -maxdepth 1 -type d -name "$m-0.1.0-*" | head -1)
    [ -n "$t" ] && [ -f "$t/bin/$m" ] || fail "M: $m's tree is not below the directory --output names" m2.log
done
echo "ok: M, --output <dir> holds each member's archive or tree under its default name"

# ── N ────────────────────────────────────────────────────────────────────────
rm -rf cli/target/dist gui/target/dist
"$MCPP" pack --mode system --workspace --exclude gui > n.log 2>&1 || fail "N: --exclude did not pack" n.log
[ -n "$(archive_of cli)" ] || fail "N: cli was not packed" n.log
[ -z "$(archive_of gui)" ] || fail "N: the member --exclude names was packed" n.log
echo "ok: N, --exclude removes a member from --workspace"

# ── O ────────────────────────────────────────────────────────────────────────
"$MCPP" pack --mode system --workspace --message-format json > o1.json 2> o1.log || fail "O: --message-format json failed" o1.log
python3 -c 'import json; json.load(open("o1.json"))' || fail "O: standard output is not one JSON document" o1.json
[ "$(python3 -c 'import json; d=json.load(open("o1.json")); print(d["kind"])')" = "mcpp.pack" ] || fail "O: the envelope kind changed" o1.json
[ "$(python3 -c 'import json; d=json.load(open("o1.json"))["data"]; print(d["stage"] is None)')" = "True" ] \
    || fail "O: data.stage is not null for several members" o1.json
got=$(members_in < o1.json)
[ "$got" = "gui cli" ] || fail "O: data.artifacts names '$got', not gui and cli in member order" o1.json
[ "$(python3 -c 'import json; d=json.load(open("o1.json"))["data"]; print(" ".join(s["member"] for s in d["stages"]))')" = "gui cli" ] \
    || fail "O: data.stages does not name one tree per member" o1.json
python3 - <<'EOF' || fail "O: a stage record lacks a field" o1.json
import json
d = json.load(open("o1.json"))["data"]
for s in d["stages"]:
    assert set(s) == {"member", "dir", "manifest", "closure"}, s
    assert s["closure"] in ("walked", "not-walked"), s
for a in d["artifacts"]:
    assert set(a) == {"path", "type", "format", "targets", "member"}, a
EOF
"$MCPP" pack --mode system -p cli --message-format json > o2.json 2> o2.log || fail "O: a pack of one member failed" o2.log
python3 - <<'EOF' || fail "O: the envelope of one member changed" o2.json
import json
d = json.load(open("o2.json"))["data"]
assert "stages" not in d, d
assert isinstance(d["stage"], dict) and set(d["stage"]) == {"dir", "manifest", "closure"}, d
assert len(d["artifacts"]) == 1 and set(d["artifacts"][0]) == {"path", "type", "format", "targets"}, d
EOF
echo "ok: O, the envelope lists every member's artifacts and keeps the single-member shape"

# ── P ────────────────────────────────────────────────────────────────────────
mkdir -p "$TMP/cfg"
cd "$TMP/cfg"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["app23", "app26"]
EOF
for a in app23 app26; do
    mkdir -p $a/src
    {
        printf '[package]\nname    = "%s"\nversion = "0.1.0"\n' "$a"
        if [ "$a" = app26 ]; then printf 'standard = "c++26"\n'; fi
        printf '\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' "$a"
    } > $a/mcpp.toml
    printf '#include <cstdio>\nint main() { std::printf("%%ld\\n", (long)__cplusplus); return 0; }\n' > $a/src/main.cpp
done
"$MCPP" pack --mode system --workspace --message-format json > p.json 2> p.log || fail "P: members of two configurations were not packed" p.log
[ "$(find target -name build.ninja -not -path '*/.build-mcpp/*' | wc -l | tr -d ' ')" = 2 ] || fail "P: expected one build directory for each configuration" p.log
[ "$(members_in < p.json)" = "app23 app26" ] || fail "P: data.artifacts does not name the two members in order" p.json
for a in app23 app26; do
    [ -n "$(find $a/target/dist -maxdepth 1 -name "$a-0.1.0-*.tar.gz" | head -1)" ] || fail "P: $a was not packed" p.log
done
echo "ok: P, members of two configurations are each packed from the build of their own"

echo "PASS: 869_a_pack_over_several_members_names_each_member_in_its_report"
