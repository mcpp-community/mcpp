#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): why Glob.EscapedSpellingIsUtf8WhateverTheName
# fails on some Windows machines. Prints; asserts nothing.
set -u
M="${MCPP_BUILT:?}"
section() { echo; echo "### $1"; }
REPO="$PWD"

section "the runner's code pages"
powershell -NoProfile -Command "[System.Text.Encoding]::Default.CodePage; (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage).ACP"

section "the unit test as CI runs it"
"$M" test test_modgraph 2>&1 | grep -E "EscapedSpelling|PASSED|FAILED|passed|failed|error" | head -20
echo "-- does the test binary embed the UTF-8 activeCodePage manifest? (mcpp.exe for comparison)"
for exe in $(find target -name 'test_modgraph*.exe' | head -1) $(find target -name 'mcpp.exe' -path '*/bin/*' | head -1); do
  printf '%s: ' "$exe"; grep -a -c "activeCodePage" "$exe" || true
done

section "the same narrow-to-path conversion under each code page"
W="$RUNNER_TEMP/codepage"; rm -rf "$W"; mkdir -p "$W"
for v in legacy utf8; do
  mkdir -p "$W/$v/src"; cd "$W/$v"
  { printf '[package]\nname = "cp_%s"\nversion = "0.1.0"\n\n[toolchain]\nwindows = "msvc@system"\n\n[targets.cp_%s]\nkind = "bin"\nmain = "src/main.cpp"\n' "$v" "$v"
    [ "$v" = utf8 ] && printf 'windows_code_page = "utf-8"\n'; } > mcpp.toml
  cat > src/main.cpp <<'EOF'
#include <windows.h>
#include <cstdio>
#include <exception>
#include <filesystem>
static void conv(unsigned cp) {
    const int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, "caf\xE9", -1, nullptr, 0);
    std::printf("  MultiByteToWideChar(cp=%u, \"caf\\xE9\") -> %s\n", cp, n ? "ok" : "fails (no mapping)");
}
int main() {
    std::printf("GetACP() = %u\n", GetACP());
    try {
        std::filesystem::path p("caf\xE9");
        std::printf("  std::filesystem::path(\"caf\\xE9\") ok, %zu UTF-16 units\n", p.native().size());
    } catch (const std::exception& e) {
        std::printf("  std::filesystem::path(\"caf\\xE9\") throws: %s\n", e.what());
    }
    for (unsigned cp : {1252u, 936u, 65001u}) conv(cp);
}
EOF
  echo "-- $v"; "$M" run 2>&1 | grep -v "^ *\(Resolved\|Target\|Inferred\|Compiling\|Finished\|Running\)" | head -12
done
cd "$REPO"
