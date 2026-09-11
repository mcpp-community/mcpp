#!/usr/bin/env bash
# requires: elf
# 644_windows_keys_are_inert_elsewhere.sh -- #618 criteria 4 and 5 on a target
# that is not PE.
#
#   4. `windows_subsystem` / `windows_entry` change no byte of an ELF artefact
#      and print nothing, so a cross-platform manifest needs no `cfg` block.
#   5. The manifest refuses the keys on a library target, naming the target and
#      the key, and a build program that names no executable of its package is
#      refused, naming the target.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
printf 'int main() { return 0; }\n' > src/main.cpp

write_manifest() {   # $1 = extra lines for [targets.inert]
    cat > mcpp.toml <<TOML
[package]
name    = "inert"
version = "0.1.0"

[targets.inert]
kind = "bin"
main = "src/main.cpp"
$1
TOML
}

artefact() { find target -type f -name inert -path '*/bin/*' | head -1; }

# ── 4. Byte-identical, and silent ─────────────────────────────────────────
write_manifest ""
"$MCPP" build --release > plain.log 2>&1 || fail "the plain build failed" plain.log
[ -n "$(artefact)" ] || fail "no artefact from the plain build" plain.log
cp "$(artefact)" "$TMP/plain.bin"

rm -rf target
write_manifest 'windows_subsystem = "windows"
windows_entry     = "wWinMain"'
"$MCPP" build --release > keyed.log 2>&1 || fail "the build with the keys failed" keyed.log
[ -n "$(artefact)" ] || fail "no artefact from the build with the keys" keyed.log
cmp "$TMP/plain.bin" "$(artefact)" || fail "the keys changed the ELF artefact" keyed.log
if grep -Eqi 'subsystem|windows_entry|unsupported key' keyed.log; then
    fail "the keys produced output on a target that is not PE" keyed.log
fi
echo "inert on ELF OK"

# The directive form is inert as well.
rm -rf target
write_manifest ""
cat > build.mcpp <<'CPP'
import mcpp;
int main() {
    mcpp::windows_subsystem("inert", "windows");
    return 0;
}
CPP
"$MCPP" build --release > directive.log 2>&1 || fail "the build with the directive failed" directive.log
cmp "$TMP/plain.bin" "$(artefact)" || fail "the directive changed the ELF artefact" directive.log
echo "directive inert on ELF OK"

# ── 5. Refusals ───────────────────────────────────────────────────────────
cat > build.mcpp <<'CPP'
import mcpp;
int main() {
    mcpp::windows_subsystem("nosuch", "windows");
    return 0;
}
CPP
if "$MCPP" build --release > refuse-directive.log 2>&1; then
    fail "a directive naming no target was accepted" refuse-directive.log
fi
grep -q 'declares no target named `nosuch`' refuse-directive.log \
    || fail "the directive refusal does not name the target" refuse-directive.log
rm -f build.mcpp

mkdir -p "$TMP/lib/src"
cd "$TMP/lib"
printf 'export module core;\nexport int core_value() { return 1; }\n' > src/core.cppm
cat > mcpp.toml <<'TOML'
[package]
name    = "lib"
version = "0.1.0"

[targets.core]
kind              = "lib"
windows_subsystem = "windows"
TOML
if "$MCPP" build > refuse-lib.log 2>&1; then
    fail "a library target accepted windows_subsystem" refuse-lib.log
fi
grep -q 'targets.core.windows_subsystem applies to an executable' refuse-lib.log \
    || fail "the refusal does not name the target and the key" refuse-lib.log
echo "refusals OK"
