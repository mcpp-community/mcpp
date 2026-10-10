#!/usr/bin/env bash
# requires: gcc elf
# 810_two_packages_deploy_the_same_bytes_to_one_name.sh -- mcpp#723, SPEC-007
# R4.2: two or more source paths for one deploy destination become ONE stage
# edge, placed when every source is byte-identical, refused at build time
# (naming every source and the destination) otherwise.
#
# The fixture is the shape #723 was filed for, without naming any plugin: a
# path dependency and its consumer each run their own build.mcpp action that
# generates a file under their own MCPP_OUT_DIR, and each deploys it to the
# SAME name (`mcpp::deploy`, docs/30-build-mcpp.md). Before this change,
# `add_deploy` (src/build/plan.cppm) refused two different source PATHS for
# one destination even when the bytes are identical, and `mcpp emit
# build-database` failed the same way.
#
# Criteria:
#   1. identical bytes: the build succeeds, and one file is placed at
#      bin/shared/shared.bin;
#   2. build.ninja carries exactly one `stage_file` edge for that destination,
#      with BOTH sources as inputs;
#   3. `mcpp emit build-database` succeeds on the same project (it used to
#      fail at planning);
#   4. different bytes: the build fails, naming both sources and the
#      destination -- not at planning, at staging.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

MCPP="${MCPP:-mcpp}"

write_dep() {   # $1 = the res.txt content
    mkdir -p "$TMP/dep/src"
    cat > "$TMP/dep/mcpp.toml" <<'TOML'
[package]
name    = "dep"
version = "0.1.0"

[targets.dep]
kind = "lib"
TOML
    cat > "$TMP/dep/src/lib.cppm" <<'CPP'
export module dep;
export int dep_value() { return 1; }
CPP
    cat > "$TMP/dep/gen.sh" <<'SH'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
SH
    chmod +x "$TMP/dep/gen.sh"
    printf '%s\n' "$1" > "$TMP/dep/res.txt"
    cat > "$TMP/dep/build.mcpp" <<'CPP'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string src  = root + "/res.txt";
    const std::string out  = std::string(mcpp::out_dir()) + "/gen/shared.bin";

    mcpp::action a;
    a.id   = "dep-gen-shared";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg(src.c_str()).arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();

    mcpp::deploy(out.c_str(), "shared");
    return 0;
}
CPP
}

write_app() {   # $1 = the res.txt content
    mkdir -p "$TMP/app/src"
    cat > "$TMP/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
dep = { path = "../dep" }
TOML
    cat > "$TMP/app/src/main.cpp" <<'CPP'
import dep;
int main() { return dep_value() == 1 ? 0 : 1; }
CPP
    cat > "$TMP/app/gen.sh" <<'SH'
#!/usr/bin/env bash
set -e
cp "$1" "$2"
SH
    chmod +x "$TMP/app/gen.sh"
    printf '%s\n' "$1" > "$TMP/app/res.txt"
    cat > "$TMP/app/build.mcpp" <<'CPP'
import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string src  = root + "/res.txt";
    const std::string out  = std::string(mcpp::out_dir()) + "/gen/shared.bin";

    mcpp::action a;
    a.id   = "app-gen-shared";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg(src.c_str()).arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();

    mcpp::deploy(out.c_str(), "shared");
    return 0;
}
CPP
}

find_graph() { find target -name build.ninja | head -1; }

# ── 1, 2, 3: identical bytes ────────────────────────────────────────────────
write_dep 'shared payload'
write_app 'shared payload'

cd "$TMP/app"
"$MCPP" build > build.log 2>&1 || fail "the build failed on identical bytes" build.log
DEPLOYED=$(find target -path '*/bin/shared/shared.bin' | head -1)
[ -n "$DEPLOYED" ] || fail "bin/shared/shared.bin was not placed" build.log
grep -qx "shared payload" "$DEPLOYED" \
    || fail "the placed file does not carry the shared bytes" "$DEPLOYED"
echo "PASS: identical bytes place one file"

G=$(find_graph)
[ -n "$G" ] || fail "no build.ninja" build.log
STAGE_LINES=$(grep -c "^build .*shared/shared\.bin : stage_file" "$G" || true)
[ "$STAGE_LINES" -eq 1 ] \
    || fail "expected exactly one stage_file edge for shared/shared.bin, found $STAGE_LINES" "$G"
STAGE_LINE=$(grep "^build .*shared/shared\.bin : stage_file" "$G")
# Each package's build program writes in the CONSUMING project's tree, in
# its own directory of this configuration's output
# (`target/.build-mcpp/out/<configuration>/<package>/...`, mcpp 2026.10.10.1+,
# 111_dep_build_mcpp.sh), so the two sources are told apart by the package
# segment.
INPUTS=$(echo "$STAGE_LINE" | sed 's/^.*: stage_file //')
[ "$(echo "$INPUTS" | wc -w)" -eq 2 ] \
    || fail "expected exactly two inputs on the stage_file edge, got: $INPUTS" "$G"
echo "$INPUTS" | tr ' ' '\n' | grep -q "\.build-mcpp/out/[0-9a-f]*/[a-z.]*dep/gen/shared\.bin$" \
    || fail "the edge does not list the dependency's source" "$G"
echo "$INPUTS" | tr ' ' '\n' | grep -q "\.build-mcpp/out/[0-9a-f]*/app/gen/shared\.bin$" \
    || fail "the edge does not also list the app's own source" "$G"
echo "PASS: one stage_file edge lists both sources"

"$MCPP" emit build-database > emit.log 2>&1 || fail "emit build-database failed on identical bytes" emit.log
echo "PASS: emit build-database succeeds"

# ── 4: different bytes -- refused at build time, naming both sources ───────
write_app 'a different payload'
touch src/main.cpp   # past the whole-project no-op fast path; see e2e 139
set +e
"$MCPP" build > collision.log 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "different bytes for one destination were accepted" collision.log
grep -q "disagree" collision.log \
    || fail "the refusal does not say the sources disagree" collision.log
grep -q '\.build-mcpp/out/[0-9a-f]*/[a-z.]*dep/gen/shared\.bin' collision.log \
    || fail "the refusal does not name the dependency's source" collision.log
grep -q '\.build-mcpp/out/[0-9a-f]*/app/gen/shared\.bin' collision.log \
    || fail "the refusal does not name the app's own source" collision.log
grep -q "shared.bin" collision.log || fail "the refusal does not name the destination" collision.log
echo "PASS: different bytes are refused at build time, naming both sources"

echo "PASS: 810_two_packages_deploy_the_same_bytes_to_one_name"
