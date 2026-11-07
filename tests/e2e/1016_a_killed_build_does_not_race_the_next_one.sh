#!/usr/bin/env bash
# requires: gcc
# A BUILD WHOSE mcpp IS KILLED DOES NOT RACE THE NEXT ONE (2026.11.7.1, design
# 2026-11-07 §4.1.2). Killing the mcpp that holds the build directory leaves
# its compilers running. The next build waited only for the directory lock, so
# it entered while those compilers still wrote BMIs and objects: 2026.11.5.1
# failed 3 of 75 such rounds under the default schedule and 12 of 15 under the
# split schedule. Every compiler is now a participant of its build's
# generation, and the next build enters only after they end. Both schedules,
# twelve rounds each: the waiting build succeeds and the build after it does
# nothing.
set -e
MCPP="${MCPP:-mcpp}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
make_project() {   # make_project <dir> <schedule>
    mkdir -p "$1/src"
    printf '[package]\nname = "kg"\nversion = "0.1.0"\nstandard = "c++23"\n[toolchain]\ndefault = "gcc@16.1.0"\n' > "$1/mcpp.toml"
    [ "$2" = split ] && printf '[build]\nbmi_schedule = "on"\n' >> "$1/mcpp.toml"
    for i in $(seq 1 24); do
        printf 'export module m%d;\nimport std;\nexport int f%d(){ std::vector<int> v(%d); return (int)v.size(); }\n' "$i" "$i" "$i" > "$1/src/m$i.cppm"
    done
    { echo 'import std;'; for i in $(seq 1 24); do echo "import m$i;"; done
      echo 'int main(){ std::println("{}", f1()+f24()); }'; } > "$1/src/main.cpp"
}
for schedule in default split; do
    dir="$TMP/$schedule"; make_project "$dir" "$schedule"
    ( cd "$dir" && "$MCPP" build > "$TMP/warm.log" 2>&1 ) || { cat "$TMP/warm.log"; exit 1; }
    for round in $(seq 1 12); do
        rm -rf "$dir/target"
        ( cd "$dir" && exec "$MCPP" build > "$TMP/a.log" 2>&1 ) & a=$!
        ( sleep 0.2; cd "$dir"; rc=0; "$MCPP" build > "$TMP/b.log" 2>&1 || rc=$?; echo "$rc" > "$TMP/b.rc" ) & b=$!
        sleep 0.9; kill -9 "$a" 2>/dev/null || true; wait "$a" 2>/dev/null || true
        wait "$b" || true
        if [ "$(cat "$TMP/b.rc")" != 0 ]; then
            echo "FAIL: $schedule round $round: the build after the killed one failed"; cat "$TMP/b.log"; exit 1
        fi
        ( cd "$dir" && "$MCPP" build > "$TMP/c.log" 2>&1 ) || { cat "$TMP/c.log"; exit 1; }
        if grep -q "Compiling" "$TMP/c.log"; then
            echo "FAIL: $schedule round $round: the following build compiled again"; cat "$TMP/c.log"; exit 1
        fi
        out=$(cd "$dir" && "$MCPP" run 2>/dev/null | tail -1)
        [ "$out" = 25 ] || { echo "FAIL: $schedule round $round: the program printed '$out'"; exit 1; }
    done
    echo "OK: $schedule: 12 killed builds, each followed by a successful build and a no-op"
done
