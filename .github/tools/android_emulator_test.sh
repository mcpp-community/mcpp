#!/usr/bin/env bash
# `mcpp test` on the x86_64 Android row, run through `adb-run` on an emulator.
#
#   android_emulator_test.sh prebuild <dir>   before the emulator: write the
#                                             fixture, provision the row and
#                                             compile its test programs
#   android_emulator_test.sh run <dir>        inside the emulator step: run
#                                             them and assert on the stream
#
# The fixture's tests measure the two properties #634 A6 decided (triage
# record §5.6), each of which failed on 2026.9.14.1 on an API 34 emulator:
#
#   runs            the program loads: on this row it needs no
#                   `libc++_shared.so`, because the engine finds the NDK's
#                   static C++ runtime by asking the driver
#   reads_relative  a file `[runtime] deploy` placed beside the program is
#                   readable there, because the runner receives
#                   MCPP_RUNTIME_FILES and `adb-run` pushes what it lists
#
# `MCPP` names the engine under test; the assertion reads the JSON test stream
# (docs/50 §8), not the human summary.
set -euo pipefail

mode="${1:?usage: android_emulator_test.sh prebuild|run <dir>}"
dir="${2:?usage: android_emulator_test.sh prebuild|run <dir>}"
: "${MCPP:?set MCPP to the engine under test}"
TARGET=x86_64-linux-android

write_fixture() {
    rm -rf "$dir"
    mkdir -p "$dir/src" "$dir/tests" "$dir/share"
    echo "m634-data" > "$dir/share/data.txt"
    cat > "$dir/mcpp.toml" <<'TOML'
[package]
name    = "droidtest"
version = "0.1.0"

[targets.droidtest]
kind = "bin"
main = "src/main.cpp"

[runtime]
deploy = [ { from = "share/data.txt", to = "data" } ]

[target.x86_64-linux-android]
min_api_level = 24
runner = ["adb-run"]

[target.x86_64-linux-android.xlings.workspace]
"xim:android-platform-tools" = "37.0.1-4"
TOML
    printf 'int main() { return 0; }\n' > "$dir/src/main.cpp"
    printf 'int main() { return 0; }\n' > "$dir/tests/runs.cpp"
    cat > "$dir/tests/reads_relative.cpp" <<'CPP'
#include <cstdio>
#include <cstring>
#include <string>
int main(int, char** argv) {
    std::string p = argv[0];
    auto s = p.rfind('/');
    std::string f = (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "/data/data.txt";
    FILE* fp = std::fopen(f.c_str(), "r");
    if (!fp) { std::printf("open failed: %s\n", f.c_str()); return 3; }
    char buf[64] = {0};
    std::fgets(buf, sizeof buf, fp);
    std::fclose(fp);
    std::printf("read %s: %s", f.c_str(), buf);
    return std::strncmp(buf, "m634-data", 9) == 0 ? 0 : 4;
}
CPP
}

status_of() {  # status_of <stream> <test>
    python3 - "$1" "$2" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("{"):
        continue
    rec = json.loads(line)
    if rec.get("test") == sys.argv[2]:
        print(rec.get("status"), rec.get("exit_code"))
        break
else:
    print("absent")
PY
}

case "$mode" in
    prebuild)
        write_fixture
        cd "$dir"
        # Compiles the tests and provisions the NDK and the platform tools. No
        # device is attached yet, so the runs are reported, not asserted.
        "$MCPP" test --target "$TARGET" --message-format json > prebuild.json 2> prebuild.err || true
        tail -5 prebuild.err
        # The property the runner step depends on, checked on the build
        # machine: no test program names the shared C++ runtime.
        found=0
        while IFS= read -r prog; do
            found=$((found + 1))
            if readelf -d "$prog" | grep -q 'NEEDED.*libc++_shared.so'; then
                echo "FAIL: $prog needs libc++_shared.so"
                exit 1
            fi
        done < <(find target -type f \( -name runs -o -name reads_relative \) -path '*/bin/*')
        [ "$found" -ge 2 ] || { echo "FAIL: expected two test programs, found $found"; exit 1; }
        echo "prebuild: $found test programs, none needs libc++_shared.so"
        ;;
    run)
        cd "$dir"
        adb devices
        "$MCPP" test --target "$TARGET" --message-format json > test.json 2> test.err || true
        tail -5 test.err
        cat test.json
        runs=$(status_of test.json runs)
        reads=$(status_of test.json reads_relative)
        echo "runs: $runs"
        echo "reads_relative: $reads"
        [ "$runs" = "pass 0" ] || { echo "FAIL: runs did not pass on the emulator"; exit 1; }
        [ "$reads" = "pass 0" ] || { echo "FAIL: reads_relative did not pass on the emulator"; exit 1; }
        echo "PASS: mcpp test on the x86_64 Android emulator"
        ;;
    *)
        echo "usage: android_emulator_test.sh prebuild|run <dir>" >&2
        exit 2
        ;;
esac
