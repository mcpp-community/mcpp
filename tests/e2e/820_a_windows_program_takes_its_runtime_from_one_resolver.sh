#!/usr/bin/env bash
# requires: windows
# 820_a_windows_program_takes_its_runtime_from_one_resolver.sh -- the runtime
# placement resolver (SPEC-006 §3.7.1, SPEC-007 R4.3 and R4.5, the 2026-09-28
# design WS1 and WS3) on a Windows host, where the program runs.
#
# The toolset's C++ runtime is the real one. A runtime search directory holds a
# synthesised set with the toolset's names (VERSIONINFO only; it is never
# loaded), older or newer than the toolset's. Until 2026.9.28.2 the plan staged
# whichever `vcruntime140.dll` a search directory offered, whatever its version
# and whatever the contract; every link warned about the difference; an action
# ran without the toolset's runtime on PATH; and a word inherited from
# `[workspace.build]` was warned once per member, naming a `[build]` table that
# does not contain it (review 2026-09-28 §2.1, §2.4).
#
#   W1  toolchain-coupled over an OLDER complete set: the toolset's set is
#       beside the program, one note states the packaging fault, no warning
#       concerns the runtime, and the program runs with Visual Studio off PATH.
#   W2  the same over a NEWER complete set: that set is placed, with one note.
#   W3  host-coupled: no copy of the runtime is placed, and the program runs.
#   W4  a runtime file declared under host-coupled is refused.
#   W5  `mcpp pack` carries, for each runtime name it packages, the file the
#       build placed.
#   W6  an action's PATH has the toolset's runtime directory first.
#   W7  a five-member workspace states an inherited redundant CRT word once,
#       naming `[workspace.build]`.
#
# Read only where the program's contract carries the toolset's runtime (the
# MSVC ABI with a redistributable directory); elsewhere it prints a READING.
set -e
source "$(dirname "$0")/_host_path.sh"
MKPE="$(cd "$(dirname "$0")" && pwd)/_synth_pe.py"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"
PY=python3
"$PY" -c "" > /dev/null 2>&1 || PY=python
"$PY" -c "" > /dev/null 2>&1 || fail "no python interpreter to synthesise the PE files with"

# rj.py <project> <query>: reads the project's resolution.json (one build per
# project directory, so there is one).
cat > rj.py <<'PY'
import glob, json, os, sys
proj, q = sys.argv[1], sys.argv[2]
files = glob.glob(os.path.join(proj, "target", "**", "resolution.json"), recursive=True)
if not files:
    sys.exit("no resolution.json under " + proj)
doc = json.load(open(files[0], encoding="utf-8"))
rt = doc["runtime"]
if q == "rule":
    print(rt["crt_set"]["rule"])
elif q == "spec":
    print(doc["toolchain"]["spec"])
elif q == "toolset":
    for p in rt["placement"]:
        if p["kind"] == "toolchain":
            print(os.path.basename(p["dest"]) + "\t" + p["sources"][0])
PY
# first_on_path.py <file> <dir>: the first entry of the PATH written to <file> is <dir>.
cat > first_on_path.py <<'PY'
import os, sys
first = open(sys.argv[1], encoding="utf-8").read().split(";")[0]
norm = lambda p: os.path.normcase(os.path.normpath(p.strip().strip('"')))
print(first)
sys.exit(0 if norm(first) == norm(sys.argv[2]) else 1)
PY

mkapp() {   # mkapp <dir> <[build] lines> <runtime search dir, or ""> [<extra tables>]
    local d="$1" build="$2" rt="$3" extra="${4:-}"
    mkdir -p "$d/src"
    printf 'import std;\nint main() { std::println("crt-ok"); return 0; }\n' > "$d/src/main.cpp"
    cat > "$d/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[build]
default-profile = "dev"
$build

$extra
EOF
    if [[ -n "$rt" ]]; then
        cat > "$d/build.mcpp" <<EOF
import mcpp;
int main() {
    mcpp::runtime_search_dir("$(host_path "$rt")");
    return 0;
}
EOF
    fi
}
bindir() { dirname "$(find "$1/target" -name app.exe -path '*/bin/*' | head -1)"; }
run_clean() { (cd "$1" && PATH="/usr/bin:/c/Windows/System32" ./app.exe 2>&1); }

# ── The row, and the toolset's set ──────────────────────────────────────────
mkapp base "" ""
(cd base && "$MCPP" build > build.log 2>&1) || fail "the baseline build failed" base/build.log
rule=$("$PY" rj.py base rule)
spec=$("$PY" rj.py base spec)
if [[ "$rule" != carry ]]; then
    echo "READING 820: the default program here does not carry the toolset's runtime (rule=$rule, toolchain=$spec)"
    echo "PASS: 820 (the row carries no toolset runtime; nothing to assert)"
    exit 0
fi
"$PY" rj.py base toolset > toolset.txt
[[ -s toolset.txt ]] || fail "the toolset's runtime set is not recorded in resolution.json" base/build.log
NAMES=$(cut -f1 toolset.txt | tr '\n' ' ')
TOOLSET_DIR=$(dirname "$(head -1 toolset.txt | cut -f2 | tr '\\' '/')")
source_of() { grep -i "^$1	" toolset.txt | head -1 | cut -f2; }
echo "READING 820: toolchain=$spec, toolset runtime=$TOOLSET_DIR: $NAMES"

mkset() {   # mkset <dir> <version>: every name of the toolset's set
    mkdir -p "$1"
    for n in $NAMES; do "$PY" "$MKPE" "$1/$n" "$2"; done
}
mkset dep-old 14.29.30139.0
mkset dep-new 14.99.65000.0

# ── W1 ──────────────────────────────────────────────────────────────────────
mkapp w1 "" "$TMP/dep-old"
(cd w1 && "$MCPP" build > build.log 2>&1) || fail "W1: the build failed" w1/build.log
B1=$(bindir w1)
for n in $NAMES; do
    cmp -s "$B1/$n" "$(source_of "$n")" \
        || fail "W1: $n beside the program is not the toolset's copy" w1/build.log
done
[[ "$(grep -c "ships the MSVC C++ runtime" w1/build.log)" -eq 1 ]] \
    || fail "W1: the dependency's runtime copy was not stated exactly once" w1/build.log
if grep -i "warning" w1/build.log | grep -qiE "runtime|\.dll"; then
    fail "W1: a warning concerns the runtime" w1/build.log
fi
out=$(run_clean "$B1") || fail "W1: the program did not run with Visual Studio off PATH: $out"
[[ "$out" == *crt-ok* ]] || fail "W1: unexpected output: $out"
echo "ok: W1 the toolset's set over an older one, one note, no warning, and the program runs"

# ── W2 ──────────────────────────────────────────────────────────────────────
mkapp w2 "" "$TMP/dep-new"
(cd w2 && "$MCPP" build > build.log 2>&1) || fail "W2: the build failed" w2/build.log
B2=$(bindir w2)
for n in $NAMES; do
    cmp -s "$B2/$n" "dep-new/$n" || fail "W2: $n is not the newer set's copy" w2/build.log
done
[[ "$(grep -c "newer than the toolset's" w2/build.log)" -eq 1 ]] \
    || fail "W2: the newer set was not stated exactly once" w2/build.log
echo "ok: W2 a newer complete set is placed, with one note"

# ── W3 ──────────────────────────────────────────────────────────────────────
mkapp w3 'cxx_runtime = "host-coupled"' "$TMP/dep-old"
(cd w3 && "$MCPP" build > build.log 2>&1) || fail "W3: the build failed" w3/build.log
B3=$(bindir w3)
for n in $NAMES; do
    [[ ! -e "$B3/$n" ]] || fail "W3: host-coupled placed $n beside the program" w3/build.log
done
grep -q "under host-coupled" w3/build.log \
    || fail "W3: the dependency's runtime copy was not stated" w3/build.log
out=$(run_clean "$B3") || fail "W3: the host-coupled program did not run: $out"
[[ "$out" == *crt-ok* ]] || fail "W3: unexpected output: $out"
echo "ok: W3 host-coupled places no copy of the runtime, and the program runs"

# ── W4 ──────────────────────────────────────────────────────────────────────
mkdir -p w4/crt
"$PY" "$MKPE" w4/crt/vcruntime140.dll 14.44.35211.0
mkapp w4 'cxx_runtime = "host-coupled"' "" '[runtime]
deploy = [ { from = "crt/vcruntime140.dll", to = "." } ]'
if (cd w4 && "$MCPP" build > build.log 2>&1); then
    fail "W4: a runtime file declared under host-coupled was accepted" w4/build.log
fi
grep -q "contract is host-coupled" w4/build.log \
    || fail "W4: the refusal does not name the contract" w4/build.log
echo "ok: W4 a runtime file declared under host-coupled is refused"

# ── W5 ──────────────────────────────────────────────────────────────────────
touch marker
pack_out=$(cd w1 && "$MCPP" pack --pkg-format dir 2>&1) || fail "W5: mcpp pack failed" <(echo "$pack_out")
DIST=$(find w1/target/dist -maxdepth 1 -mindepth 1 -newer marker | head -1)
[[ -d "$DIST" ]] || fail "W5: no package directory" <(echo "$pack_out"; find w1/target/dist)
packed=0
while IFS= read -r f; do
    n=$(basename "$f")
    cmp -s "$f" "$B1/$n" || fail "W5: the package's $n is not the file the build placed" <(echo "$pack_out")
    packed=$((packed + 1))
done < <(find "$DIST" -type f \( -iname 'vcruntime140*.dll' -o -iname 'msvcp140*.dll' \))
[[ "$packed" -ge 1 ]] || fail "W5: the package carries no runtime file" <(echo "$pack_out"; find "$DIST")
echo "ok: W5 the package carries the build's runtime files ($packed)"

# ── W6 ──────────────────────────────────────────────────────────────────────
PYW=$(host_path "$(command -v "$PY")")
[[ "$PYW" == *.exe ]] || PYW="$PYW.exe"
mkdir -p w6/src
printf 'int main() { return 0; }\n' > w6/src/main.cpp
printf '[package]\nname    = "act"\nversion = "0.1.0"\n' > w6/mcpp.toml
cat > w6/build.mcpp <<EOF
import std;
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/path.txt";
    mcpp::action a;
    a.id   = "report-path";
    a.role = mcpp::roles::check;
    a.arg("$PYW").arg("-c")
     .arg("import os,sys; open(sys.argv[1],'w').write(os.environ.get('PATH',''))")
     .arg(out.c_str())
     .output(out.c_str())
     .submit();
    return 0;
}
EOF
(cd w6 && "$MCPP" build > build.log 2>&1) || fail "W6: the build failed" w6/build.log
PATHFILE=$(find w6/target -name path.txt | head -1)
[[ -n "$PATHFILE" ]] || fail "W6: the action wrote no PATH" w6/build.log
first=$("$PY" first_on_path.py "$PATHFILE" "$TOOLSET_DIR") \
    || fail "W6: the action's PATH begins with '$first', not the toolset's runtime directory $TOOLSET_DIR"
echo "ok: W6 the action's PATH begins with the toolset's runtime directory"

# ── W7 ──────────────────────────────────────────────────────────────────────
WORD="-fms-runtime-lib=dll"
[[ "$spec" == msvc* ]] && WORD="/MD"
mkdir -p ws
cat > ws/mcpp.toml <<EOF
[workspace]
members = ["m1", "m2", "m3", "m4", "m5"]

[workspace.build]
cxxflags = ["$WORD"]
EOF
for i in 1 2 3 4 5; do
    mkdir -p "ws/m$i/src"
    printf '[package]\nname    = "m%s"\nversion = "0.1.0"\n' "$i" > "ws/m$i/mcpp.toml"
    printf 'int main() { return 0; }\n' > "ws/m$i/src/main.cpp"
done
(cd ws && "$MCPP" build > build.log 2>&1) || fail "W7: the workspace build failed" ws/build.log
said=$(grep -c "agrees with the CRT model" ws/build.log || true)
[[ "$said" -eq 1 ]] || fail "W7: the inherited word was stated $said time(s), not once" ws/build.log
grep "agrees with the CRT model" ws/build.log | grep -qF "[workspace.build] cxxflags" \
    || fail "W7: the statement does not name [workspace.build] cxxflags" ws/build.log
echo "ok: W7 an inherited redundant word is stated once, at [workspace.build]"

echo "PASS: 820_a_windows_program_takes_its_runtime_from_one_resolver"
