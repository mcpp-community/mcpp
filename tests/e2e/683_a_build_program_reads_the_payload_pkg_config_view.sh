#!/usr/bin/env bash
# 683 -- `mcpp::pkg_config_libdir()` names the pkg-config directories of the
# registry SubOS, the view payload recipes declare their `.pc` files into
# (#634, A7). It is an accessor and not an environment default, so the host's
# pkg-config database is untouched for a package that means it.
#
# Legs:
#   A. The value is the registry SubOS's `usr/lib/pkgconfig` and
#      `usr/share/pkgconfig`, in that order.
#   B. The build program's environment does not carry PKG_CONFIG_LIBDIR.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
printf '[package]\nname = "app"\nversion = "0.1.0"\n' > mcpp.toml
printf 'int main() { return 0; }\n' > src/main.cpp
cat > build.mcpp <<'CPP'
import std;
import mcpp;
int main() {
    std::string line = std::string("libdir=") + mcpp::pkg_config_libdir();
    const char* inherited = std::getenv("PKG_CONFIG_LIBDIR");
    line += std::string(" inherited=") + (inherited ? inherited : "(unset)");
    mcpp::warning(line.c_str());
    return 0;
}
CPP
env -u PKG_CONFIG_LIBDIR "$MCPP" build > build.log 2>&1 || fail "build failed" build.log

registry=$("$MCPP" self env 2>/dev/null | sed -n 's/^xlings home *= *//p' | head -1)
[ -n "$registry" ] || fail "cannot read the registry from mcpp self env"
# `self env` prints the native spelling (`C:\Users\...` on Windows); the
# accessor joins generic, `/`-separated paths with the platform's list
# separator, which the `?` below matches.
registry=${registry//\\//}
line=$(grep -o 'libdir=.*' build.log | head -1)
case "$line" in
    "libdir=$registry/subos/default/usr/lib/pkgconfig"?"$registry/subos/default/usr/share/pkgconfig inherited=(unset)") ;;
    *) fail "A/B: the build program read '$line'" build.log ;;
esac

echo "OK"
