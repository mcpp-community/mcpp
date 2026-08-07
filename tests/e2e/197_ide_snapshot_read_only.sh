#!/usr/bin/env bash
# requires:
# Prove `ide snapshot` leaves both the project and an isolated MCPP_HOME untouched.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PROJECT="$TMP/project"
IDE_HOME="$TMP/mcpp-home"
mkdir -p "$PROJECT/src" "$IDE_HOME"

cat >"$PROJECT/mcpp.toml" <<'EOF'
[package]
name = "read_only_probe"
version = "1.0.0"

[dependencies]
"missing.remote" = "99.99.99"

[generated_files]
"src/generated.cpp" = "int generated_value() { return 42; }\n"

[toolchain]
linux = "llvm@99.99.99"
macosx = "llvm@99.99.99"
windows = "llvm@99.99.99"
EOF

cat >"$PROJECT/build.mcpp" <<'EOF'
#include <fstream>
int main() {
    std::ofstream("build-mcpp-ran") << "snapshot executed build.mcpp\n";
    return 0;
}
EOF

cat >"$PROJECT/src/main.cpp" <<'EOF'
int main() { return 0; }
EOF

inventory() {
    local root=$1 output=$2
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$root" >"$output" <<'PY'
import hashlib
import os
import stat
import sys

root = os.path.abspath(sys.argv[1])
records = [("D", ".", "-")]
for current, directories, files in os.walk(root, followlinks=False):
    directories.sort()
    files.sort()
    for name in directories + files:
        path = os.path.join(current, name)
        relative = os.path.relpath(path, root).replace(os.sep, "/")
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            records.append(("L", relative, os.readlink(path)))
        elif stat.S_ISDIR(info.st_mode):
            records.append(("D", relative, "-"))
        elif stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with open(path, "rb") as stream:
                for block in iter(lambda: stream.read(65536), b""):
                    digest.update(block)
            records.append(("F", relative, digest.hexdigest()))
        else:
            records.append(("O", relative, str(info.st_mode)))
for record in sorted(records, key=lambda item: item[1]):
    print("\t".join(record))
PY
        return
    fi

    if ! command -v sha256sum >/dev/null 2>&1 \
       && ! command -v shasum >/dev/null 2>&1; then
        echo "python3, sha256sum, or shasum is required" >&2
        return 1
    fi
    (
        cd "$root"
        find . -print | LC_ALL=C sort | while IFS= read -r path; do
            if [[ -L "$path" ]]; then
                printf 'L\t%s\t%s\n' "$path" "$(readlink "$path")"
            elif [[ -d "$path" ]]; then
                printf 'D\t%s\t-\n' "$path"
            elif [[ -f "$path" ]]; then
                if command -v sha256sum >/dev/null 2>&1; then
                    digest=$(sha256sum "$path" | awk '{print $1}')
                else
                    digest=$(shasum -a 256 "$path" | awk '{print $1}')
                fi
                printf 'F\t%s\t%s\n' "$path" "$digest"
            else
                printf 'O\t%s\t-\n' "$path"
            fi
        done
    ) >"$output"
}

inventory "$PROJECT" "$TMP/project.before"
inventory "$IDE_HOME" "$TMP/home.before"

(cd "$PROJECT" && MCPP_HOME="$IDE_HOME" "$MCPP" ide snapshot --format json) \
    >"$TMP/default.json" 2>"$TMP/default.err"
(cd "$PROJECT" && MCPP_HOME="$IDE_HOME" "$MCPP" ide snapshot \
    --target imaginary-unknown-none --profile dev --features alpha,beta \
    --cap ssl=missing --include-dev-dependencies) \
    >"$TMP/selectors.json" 2>"$TMP/selectors.err"

[[ ! -s "$TMP/default.err" ]] || { cat "$TMP/default.err"; exit 1; }
[[ ! -s "$TMP/selectors.err" ]] || { cat "$TMP/selectors.err"; exit 1; }
grep -q '"kind": "mcpp.ide.snapshot"' "$TMP/default.json"
grep -q '"state": "partial"' "$TMP/default.json"
grep -q '"target": "imaginary-unknown-none"' "$TMP/selectors.json"
grep -q '"includeDevDependencies": true' "$TMP/selectors.json"

inventory "$PROJECT" "$TMP/project.after"
inventory "$IDE_HOME" "$TMP/home.after"
cmp -s "$TMP/project.before" "$TMP/project.after" || {
    diff -u "$TMP/project.before" "$TMP/project.after" || true
    echo "project tree changed" >&2
    exit 1
}
cmp -s "$TMP/home.before" "$TMP/home.after" || {
    diff -u "$TMP/home.before" "$TMP/home.after" || true
    echo "isolated MCPP_HOME changed" >&2
    exit 1
}

for path in \
    "$PROJECT/.mcpp" \
    "$PROJECT/.xlings.json" \
    "$PROJECT/mcpp.lock" \
    "$PROJECT/target" \
    "$PROJECT/compile_commands.json" \
    "$PROJECT/src/generated.cpp" \
    "$PROJECT/build-mcpp-ran"; do
    [[ ! -e "$path" ]] || { echo "unexpected artifact: $path" >&2; exit 1; }
done
[[ -z "$(find "$IDE_HOME" -mindepth 1 -print -quit)" ]] || {
    echo "isolated MCPP_HOME is not empty" >&2
    find "$IDE_HOME" -mindepth 1 -print >&2
    exit 1
}

echo "OK"
