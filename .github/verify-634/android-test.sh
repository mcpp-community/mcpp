#!/usr/bin/env bash
# M2 (Android half): does `mcpp test --target x86_64-linux-android` run test
# programs on an emulator through `adb-run`, and can a test read a deployed
# file beside itself, or the build machine's absolute path?
#
#   android-test.sh prebuild   before the emulator: provision and compile
#   android-test.sh run        inside the emulator step
set +e
set -u
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/test-fixture.sh"

D="$RUNNER_TEMP/m2android/ttest"
ROW='[target.x86_64-linux-android]
min_api_level = 24
runner = ["adb-run"]

[target.x86_64-linux-android.xlings.workspace]
"xim:android-platform-tools" = "37.0.1-3"'

case "${1:-}" in
    prebuild)
        make_test_fixture "$D" "$ROW"
        cd "$D" || exit 1
        # Compiles the tests and provisions the NDK and platform-tools; the
        # host cannot execute them, which is reported, not failed.
        "$MCPP" test --target x86_64-linux-android --message-format json > prebuild.json 2> prebuild.err
        reading m2android.prebuild "exit=$? stderr=$(tail -4 prebuild.err | tr '\n' '|' | cut -c1-400)"
        report_tests prebuild.json m2android.prebuild
        ;;
    run)
        cd "$D" || exit 1
        adb devices > devices.txt 2>&1
        reading m2android.devices "$(one_line devices.txt)"
        "$MCPP" test --target x86_64-linux-android --message-format json > test.json 2> test.err
        reading m2android.exit "exit=$? stderr=$(tail -5 test.err | tr '\n' '|' | cut -c1-500)"
        report_tests test.json m2android
        ;;
    *) echo "usage: android-test.sh prebuild|run"; exit 1 ;;
esac
exit 0
