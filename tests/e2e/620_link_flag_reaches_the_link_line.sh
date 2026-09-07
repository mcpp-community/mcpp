#!/usr/bin/env bash
# requires: gcc
# A linker flag a build program COMPUTED reaches the link line.
#
# `link-lib`, `link-search` and `link-script` each name one KIND of thing, so a
# flag the program worked out for itself -- a generated version script,
# `-Wl,--wrap=`, `-Wl,--exclude-libs` -- had no way out of build.mcpp. This is
# that outlet.
#
# THE CRITERION IS THE LINKER'S BEHAVIOUR, NOT THE COMMAND LINE. Asserting that
# a string appears in build.ninja would pass for a flag that was written down
# and never handed to the linker, and it would break the day the flag is
# rendered with different spacing. `-Wl,--defsym=<sym>=<value>` DEFINES a
# symbol at link time, so the program's own output is the evidence: the value
# can only be there if the linker saw the flag.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'TOML'
[package]
name    = "linkflag"
version = "0.1.0"

[targets.linkflag]
kind = "bin"
main = "src/main.cpp"
TOML

# The program computes the value rather than hard-coding it, because a value
# the manifest could have written is a case `[build] ldflags` already served.
cat > build.mcpp <<'CPP'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    int computed = 40 + 2;
    std::string flag = "-Wl,--defsym=mcpp_e2e_620=" + std::to_string(computed);
    mcpp::link_flag(flag.c_str());
    return 0;
}
CPP

cat > src/main.cpp <<'CPP'
#include <cstdio>
extern "C" char mcpp_e2e_620;
int main() {
    // The linker put the value in the SYMBOL'S ADDRESS, which is how --defsym
    // works; reading the object would read memory that was never written.
    std::printf("defsym=%lld\n",
                (long long)(unsigned long long)(void*)&mcpp_e2e_620);
    return 0;
}
CPP

if ! out=$("$MCPP" run 2>&1); then
  echo "$out"
  echo "FAIL: the project did not build or run"
  exit 1
fi
echo "$out"

case "$out" in
  *"defsym=42"*) ;;
  *) echo "FAIL: the computed link flag did not reach the linker"; exit 1 ;;
esac

echo "PASS: a link flag computed by build.mcpp reaches the link line"
