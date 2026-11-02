#!/usr/bin/env bash
# requires: pack
# 1008 -- a format a package provides is told the compression the command or
# `[pack] compress` stated (MCPP_PACK_COMPRESS, `mcpp::pack_compress()`), and
# decides what its container does with it (2026.11.2.3).
#
# `--compress` was refused with every provided format ("writes none"), so a
# provider whose container can be compressed --- openkal-platform's okb, which
# stores its images compressed under `gzip` --- had no way to learn it.
#
# Legs:
#   A. `--pkg-format probe --compress gzip:9`: the program reads `gzip:9`.
#   B. `--pkg-format probe`: it reads nothing.
#   C. `[pack] compress = "zstd"`: it reads `zstd`.
#   D. `--pkg-format dir --compress gzip` is still refused: a directory writes
#      no container.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p app/src
cat > app/mcpp.toml <<'EOF2'
[package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF2
printf 'int main() { return 0; }\n' > app/src/main.cpp
cat > app/build.mcpp <<'EOF2'
import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("probe");
    if (std::string_view(mcpp::pack_format()) != "probe") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/probe.txt";
    const std::string text = std::string("compress=") + mcpp::pack_compress();
    mcpp::action a;
    a.id = "probe";
    a.role = "artifact";
    a.description = "probe";
    a.arg("/bin/sh").arg("-c").arg(("printf '%s' '" + text + "' > " + out).c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
EOF2
cd app

# ── A ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --pkg-format probe --compress gzip:9 > a.log 2>&1 || fail "A: pack failed" a.log
probe=$(find target -name probe.txt | head -1)
[ "$(cat "$probe")" = "compress=gzip:9" ] || fail "A: the program read '$(cat "$probe" 2>/dev/null)'" a.log
echo "ok: A, a provided format is told the compression the command states"

# ── B ───────────────────────────────────────────────────────────────────────
"$MCPP" pack --pkg-format probe > b.log 2>&1 || fail "B: pack failed" b.log
[ "$(cat "$probe")" = "compress=" ] || fail "B: with none stated the program read '$(cat "$probe")'" b.log
echo "ok: B, and nothing when none is stated"

# ── C ───────────────────────────────────────────────────────────────────────
printf '\n[pack]\ncompress = "zstd"\n' >> mcpp.toml
"$MCPP" pack --pkg-format probe > c.log 2>&1 || fail "C: pack failed" c.log
[ "$(cat "$probe")" = "compress=zstd" ] || fail "C: under [pack] compress the program read '$(cat "$probe")'" c.log
echo "ok: C, and the one [pack] compress states"

# ── D ───────────────────────────────────────────────────────────────────────
if "$MCPP" pack --pkg-format dir --compress gzip > d.log 2>&1; then fail "D: a directory was compressed" d.log; fi
grep -q "writes none" d.log || fail "D: the refusal" d.log
echo "ok: D, a directory writes no container and is refused"
