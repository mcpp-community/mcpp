#!/usr/bin/env bash
# M2 (iOS simulator half): does `mcpp test --target aarch64-ios-sim` run test
# programs through `simctl-run`, and can a test read a deployed file beside
# itself, or the build machine's absolute path?
set +e
set -u
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/test-fixture.sh"

if ! xcrun --sdk iphonesimulator --show-sdk-path > /dev/null 2>&1; then
    reading m2ios.skip "no iphonesimulator SDK on this runner"; exit 0
fi
D="$RUNNER_TEMP/m2ios/ttest"
make_test_fixture "$D" '[build]
ios_deployment_target = "17.0"

[target.aarch64-ios-sim]
runner = ["simctl-run"]

[target.aarch64-ios-sim.xlings.workspace]
"xim:apple-simulator-tools" = "0.2.0"'
cd "$D" || exit 1
"$MCPP" test --target aarch64-ios-sim --message-format json > test.json 2> test.err
rc=$?; reading m2ios.exit "exit=$rc stderr=$(tail -5 test.err | tr '\n' '|' | cut -c1-500)"
[ "$rc" -eq 0 ] || tail -40 test.err
reading m2ios.deployed-in-build-tree "$(find target -path '*bin/data/data.txt' 2>/dev/null | head -1)"
report_tests test.json m2ios
exit 0
