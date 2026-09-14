#!/usr/bin/env bash
# M3: on a fresh home that declares only `xim:gtk4`, is the SubOS pkg-config
# view complete, and for every module it lacks, is the payload installed and
# does its recipe declare its `.pc` files?
set +e
set -u
. "$(dirname "$0")/common.sh"

export MCPP_HOME="$RUNNER_TEMP/home-m3"
rm -rf "$MCPP_HOME"; mkdir -p "$MCPP_HOME"
"$MCPP" self config --mirror GLOBAL > /dev/null 2>&1
D="$RUNNER_TEMP/m3/gtkview"
rm -rf "$D"; mkdir -p "$D/src"
cat > "$D/mcpp.toml" <<'T'
[package]
name    = "gtkview"
version = "0.1.0"

[xlings.workspace]
"xim:gtk4" = "4.16.13"
T
printf 'int main() { return 0; }\n' > "$D/src/main.cpp"
cd "$D" || exit 1
"$MCPP" build > build.log 2>&1
reading m3.build "exit=$? $(tail -3 build.log | tr '\n' '|' | cut -c1-400)"
"$MCPP" self env > env.txt 2>&1
reading m3.xlings "$(grep -m1 'xlings pinned' env.txt)"

REG="$MCPP_HOME/registry"
V="$REG/subos/default/usr/lib/pkgconfig"
reading m3.view "entries=$(ls "$V" 2>/dev/null | wc -l) symlinks=$(find "$V" -maxdepth 1 -type l 2>/dev/null | wc -l)"
reading m3.payloads "installed=$(ls "$REG/data/xpkgs" 2>/dev/null | wc -l)"

out=$(env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$V" pkg-config --cflags --libs gtk4 2>&1); rc=$?
reading m3.pkg-config "exit=$rc $(printf '%s' "$out" | head -c 300 | tr '\n' '|')"
missing=$(printf '%s\n' "$out" | sed -n "s/^Package '\([^']*\)', required by .*/\1/p; s/^Package \([^ ]*\) was not found.*/\1/p" | sort -u)
for m in $missing; do
    pc=$(find "$REG/data/xpkgs" -path "*/pkgconfig/$m.pc" 2>/dev/null | head -1)
    payload=$(printf '%s' "$pc" | sed -n 's|.*/xpkgs/\([^/]*\)/\([^/]*\)/.*|\1@\2|p')
    name=${payload#*-x-}; name=${name%@*}
    recipe=$(find "$REG/data/xim-pkgindex/pkgs" -name "$name.lua" 2>/dev/null | head -1)
    declares=$(grep -c 'declare_pkgconfig' "$recipe" 2>/dev/null)
    reading m3.missing "$m: pc=${pc:+found} payload=${payload:-not-installed} recipe=${recipe:+present} declares_pkgconfig=${declares:-0}"
done

# The same question for every installed payload that ships `.pc` files:
# is each of its `.pc` names present in the view?
absent=""
for pc in $(find "$REG/data/xpkgs" -path '*/lib/pkgconfig/*.pc' 2>/dev/null); do
    b=$(basename "$pc")
    [ -e "$V/$b" ] || absent="$absent $(printf '%s' "$pc" | sed -n 's|.*/xpkgs/\([^/]*\)/.*|\1|p'):$b"
done
reading m3.pc-not-in-view "$(echo "$absent" | tr ' ' '\n' | sort -u | tr '\n' ' ' | cut -c1-900)"
exit 0
