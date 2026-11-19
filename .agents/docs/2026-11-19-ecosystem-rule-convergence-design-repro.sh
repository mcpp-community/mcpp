#!/usr/bin/env bash
# Reproductions for 2026-11-19-ecosystem-rule-convergence-design.md.
#
#   MCPP=<abs path of mcpp> bash <this file> [case...]
#
# Cases: 960 956 952 952s 931 931d 954 958 955 (all when none is named). Each prints what it
# observed and a line `RESULT <case>: <reproduced|not reproduced|skipped: why>`.
# Nothing is asserted about a fixed engine: the script states the behaviour of
# the engine it is given. Measured with mcpp 2026.11.18.1 on Linux x86_64.
#
#   956 needs INDEX_830=<a checkout of mcpplibs/mcpp-index at 333be93>, the
#       commit that lists openkal-macos 0.23.5; skipped without it.
#   952 needs `unshare -Urpf` (a new pid namespace); skipped without it.
set -u

MCPP="${MCPP:?set MCPP to the absolute path of an mcpp binary}"
JOBS="${MCPP_JOBS:-8}"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/mcpp-issues-XXXXXX")"
echo "work directory: $ROOT ($("$MCPP" --version))"

run() { MCPP_JOBS="$JOBS" "$MCPP" "$@"; }

git_init() {   # a publish archive is built from git objects
    git -C "$1" init -q && git -C "$1" add -A &&
        git -C "$1" -c user.name=probe -c user.email=probe@example.invalid commit -qm init
}

# 960: the descriptor `mcpp publish` writes, read by `xpkg parse` and by a
# consumer through a local index; then the same archive as the string form.
case_960() {
    local d="$ROOT/960"; mkdir -p "$d/lib/src" "$d/index/pkgs/p" "$d/app/src"
    printf '[package]\nnamespace = "probe"\nname = "sdk"\nversion = "0.1.0"\ndescription = "probe"\nlicense = "MIT"\nrepo = "https://github.com/example/probe"\n\n[targets.sdk]\nkind = "lib"\n' > "$d/lib/mcpp.toml"
    printf 'export module probe.sdk;\nexport int sdk_value() { return 42; }\n' > "$d/lib/src/sdk.cppm"
    git_init "$d/lib"
    (cd "$d/lib" && run publish --dry-run >/dev/null 2>&1)
    local lua="$d/lib/target/dist/probe.sdk.lua" tar="$d/lib/target/dist/probe.sdk-0.1.0.tar.gz"
    [ -f "$lua" ] && [ -f "$tar" ] || { echo "RESULT 960: skipped: publish wrote no descriptor"; return; }
    echo "-- xpkg parse of what publish wrote:"
    local parsed; parsed="$(run xpkg parse "$lua" 2>&1)"; echo "$parsed" | tail -2
    cp "$lua" "$d/index/pkgs/p/"
    local store="$d/app/.mcpp/.xlings/data/xpkgs/probe.sdk/0.1.0"; mkdir -p "$store"
    tar -xzf "$tar" -C "$store"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[indices]\nprobe = { path = "%s" }\n\n[dependencies.probe]\nsdk = "0.1.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$d/index" > "$d/app/mcpp.toml"
    printf 'import std;\nimport probe.sdk;\nint main() { std::println("v={}", sdk_value()); }\n' > "$d/app/src/main.cpp"
    echo "-- a consumer of that descriptor:"
    local inlineOut; inlineOut="$(cd "$d/app" && run build 2>&1 | tail -1)"; echo "$inlineOut"
    # The string form: the field replaced, nothing else.
    awk 'BEGIN{skip=0} /^    mcpp = \{/{print "    mcpp = \"*/mcpp.toml\","; skip=1; next} skip&&/^    \},/{skip=0; next} !skip{print}' \
        "$lua" > "$d/index/pkgs/p/probe.sdk.lua"
    echo "-- the same archive, the field as a string:"
    local stringOut; stringOut="$(cd "$d/app" && run run 2>&1 | tail -1)"; echo "$stringOut"
    if echo "$inlineOut" | grep -q 'missing sources' && [ "$stringOut" = "v=42" ]; then
        echo "RESULT 960: reproduced"
    else echo "RESULT 960: not reproduced"; fi
}

# 956: mcpplibs.os as a build dependency under --target x86_64-macos-musl, with
# an index that lists a newer patch of a member of the platform's version group.
case_956() {
    local idx="${INDEX_830:-}"
    [ -n "$idx" ] && [ -f "$idx/pkgs/o/openkal-macos.lua" ] || { echo "RESULT 956: skipped: INDEX_830 is not set"; return; }
    local d="$ROOT/956"; mkdir -p "$d/dep/src" "$d/build/src"
    for kind in dep build; do
        local table="dependencies"; [ "$kind" = build ] && table="build-dependencies"
        printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[indices]\nmcpplibs = { path = "%s" }\n\n[%s]\nmcpplibs.os = "0.4.4"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$idx" "$table" > "$d/$kind/mcpp.toml"
        printf 'import std;\nint main() { std::println("hi"); }\n' > "$d/$kind/src/main.cpp"
    done
    printf 'import std;\nimport mcpp;\nint main() { return 0; }\n' > "$d/build/build.mcpp"
    echo "-- as a dependency (the group binds the range):"
    (cd "$d/dep" && run build --target x86_64-macos-musl --configure-only 2>&1 | grep -E 'Resolved openkal-macos|^error' | sort -u)
    echo "-- as a build dependency:"
    local out; out="$(cd "$d/build" && run build --target x86_64-macos-musl --configure-only 2>&1)"
    echo "$out" | grep -E 'Resolved openkal-(macos|linux)|^error|requested by'
    echo "-- the same manifest for the host (which table of mcpplibs.os is read):"
    (cd "$d/build" && run build --configure-only 2>&1 | grep -E 'Resolved openkal-(macos|linux)|^error')
    if echo "$out" | grep -q 'is a version group that names'; then echo "RESULT 956: reproduced"
    else echo "RESULT 956: not reproduced"; fi
}

# 952: a build ended by SIGKILL inside a new pid namespace, continued from this
# one; then the same after `mcpp clean`; and a control in one namespace.
case_952() {
    unshare -Urpf --mount-proc true 2>/dev/null || { echo "RESULT 952: skipped: no new pid namespace here"; return; }
    local d="$ROOT/952"; mkdir -p "$d/seed/src"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/seed/mcpp.toml"
    local main='import std;\n' body='int main() { int s = 0;'
    for i in $(seq 0 23); do
        printf 'import std;\nint f%s(int x) { std::vector<std::string> v; for (int i = 0; i < x; ++i) v.push_back(std::format("{}", i)); std::ranges::sort(v); return (int)v.size(); }\n' "$i" > "$d/seed/src/u$i.cpp"
        main="$main""int f$i(int);\n"; body="$body s += f$i(3);"
    done
    printf "$main$body"' std::println("{}", s); }\n' > "$d/seed/src/main.cpp"
    pending() { local n=0 a; for a in "$1"/.mcpp/compiler-runs/*/run-*/attempts/*; do
        [ -e "$a/admitted" ] && [ ! -e "$a/retired" ] && n=$((n+1)); done; echo "$n"; }
    local hit="" t
    for t in 0.8 1.0 1.4 1.8 2.2 2.6; do
        local p="$d/ns-$t"; mkdir -p "$p"; cp -r "$d/seed/src" "$d/seed/mcpp.toml" "$p/"
        (cd "$p" && unshare -Urpf --mount-proc timeout -s KILL "$t" env MCPP_JOBS="$JOBS" "$MCPP" build >/dev/null 2>&1)
        if [ "$(pending "$p")" -gt 0 ]; then hit="$p"; echo "-- killed after ${t}s: $(pending "$p") admitted attempts without retirement"; break; fi
    done
    [ -n "$hit" ] || { echo "RESULT 952: skipped: no kill landed during a compile"; return; }
    echo "-- the next build, from this pid namespace:"
    local out; out="$(cd "$hit" && run build 2>&1 | tail -1)"; echo "$out"
    echo "-- after mcpp clean:"
    (cd "$hit" && run clean 2>&1 | tail -1; run build 2>&1 | tail -1)
    echo "-- control: killed and continued in one namespace:"
    local c="$d/control" ok=""
    for t in 0.8 1.0 1.4 1.8; do
        mkdir -p "$c-$t"; cp -r "$d/seed/src" "$d/seed/mcpp.toml" "$c-$t/"
        (cd "$c-$t" && timeout -s KILL "$t" env MCPP_JOBS="$JOBS" "$MCPP" build >/dev/null 2>&1) 2>/dev/null
        if [ "$(pending "$c-$t")" -gt 0 ]; then sleep 2; (cd "$c-$t" && run build 2>&1 | tail -1); ok=1; break; fi
    done
    [ -n "$ok" ] || echo "(no kill landed during a compile)"
    if echo "$out" | grep -q 'occupied or unknown'; then echo "RESULT 952: reproduced"
    else echo "RESULT 952: not reproduced"; fi
}

# 931: a file a link flag names as the value of an option changes; the program
# is relinked or it is not. Control: the same file named as a response file.
case_931() {
    local d="$ROOT/931"
    link_stamp() { stat -c %Y "$1"/target/*/*/bin/app 2>/dev/null | head -1; }
    probe() {   # <dir> <flag form with %s for the file>
        mkdir -p "$1/src"; printf '{ main; };\n' > "$1/list.txt"
        local flag; flag="$(printf -- "$2" "$1/list.txt")"
        printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[build]\nldflags = ["%s"]\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$flag" > "$1/mcpp.toml"
        printf 'int helper() { return 1; }\nint main() { return helper() - 1; }\n' > "$1/src/main.cpp"
        (cd "$1" && run build >/dev/null 2>&1) || { echo "build-failed"; return; }
        local before; before="$(link_stamp "$1")"; sleep 1.1
        printf '{ main; helper; };\n' > "$1/list.txt"
        (cd "$1" && run build >/dev/null 2>&1)
        [ "$(link_stamp "$1")" = "$before" ] && echo "not relinked" || echo "relinked"
    }
    local asOption; asOption="$(probe "$d/option" '-Wl,--dynamic-list=%s')"
    echo "-- the file as the value of an option (-Wl,--dynamic-list=<file>): $asOption"
    local asRsp; asRsp="$(probe_rsp "$d/rsp")"
    echo "-- control, the file as a response file (@<file>): $asRsp"
    [ "$asOption" = "not relinked" ] && echo "RESULT 931: reproduced" || echo "RESULT 931: not reproduced"
}
probe_rsp() {
    mkdir -p "$1/src"; printf -- '-Wl,--as-needed\n' > "$1/args.rsp"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[build]\nldflags = ["@%s"]\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$1/args.rsp" > "$1/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$1/src/main.cpp"
    (cd "$1" && run build >/dev/null 2>&1) || { echo "build-failed"; return; }
    local before; before="$(stat -c %Y "$1"/target/*/*/bin/app | head -1)"; sleep 1.1
    printf -- '-Wl,--no-as-needed\n' > "$1/args.rsp"
    (cd "$1" && run build >/dev/null 2>&1)
    [ "$(stat -c %Y "$1"/target/*/*/bin/app | head -1)" = "$before" ] && echo "not relinked" || echo "relinked"
}

# 954: an abi table stated once at a virtual workspace root; the warning meant
# for a dependency's own table is counted.
case_954() {
    local d="$ROOT/954"; mkdir -p "$d/a/src" "$d/b/src"
    printf '[workspace]\nmembers = ["a", "b"]\n\n[workspace.target.'"'"'cfg(linux)'"'"'.abi]\nexceptions = true\n' > "$d/mcpp.toml"
    printf '[package]\nname = "a"\nversion = "0.1.0"\n\n[targets.a]\nkind = "lib"\n' > "$d/a/mcpp.toml"
    printf 'export module a;\nexport int a_value() { return 1; }\n' > "$d/a/src/a.cppm"
    printf '[package]\nname = "b"\nversion = "0.1.0"\n\n[dependencies]\na = { path = "../a" }\n\n[targets.b]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/b/mcpp.toml"
    printf 'import a;\nint main() { return a_value() - 1; }\n' > "$d/b/src/main.cpp"
    local out; out="$(cd "$d" && run build --workspace --configure-only 2>&1)"
    echo "$out" | grep -E 'declares \[target|^error' | sort | uniq -c
    local n; n="$(echo "$out" | grep -c 'declares \[target')"
    [ "$n" -gt 0 ] && echo "RESULT 954: reproduced ($n warnings, no member has an abi table)" || echo "RESULT 954: not reproduced"
}

# 958: a compile command of more than 512 argument words.
case_958() {
    local d="$ROOT/958"; mkdir -p "$d/src"
    local defs="" i
    for i in $(seq 0 519); do defs="$defs\"PROBE_D$i=1\", "; done
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[build]\ndefines = [%s]\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "${defs%, }" > "$d/mcpp.toml"
    printf 'int main() { return PROBE_D519 - 1; }\n' > "$d/src/main.cpp"
    local out; out="$(cd "$d" && run build 2>&1 | grep -E '^cannot start|Finished' | cut -c1-200)"; echo "$out"
    if echo "$out" | grep -qi 'out of memory'; then echo "RESULT 958: reproduced (520 defines)"
    else echo "RESULT 958: not reproduced"; fi
}

# 955: where a plugin's own host program is visible. `plug` has a library and
# a program `steps`; its library asks for the program's path.
case_955() {
    local d="$ROOT/955"; mkdir -p "$d/plug/src" "$d/plug/tools" "$d/fw/src" "$d/direct/src" "$d/through/src"
    printf '[package]\nname = "plug"\nversion = "0.1.0"\n\n[targets.plug]\nkind = "lib"\n\n[targets.steps]\nkind = "bin"\nmain = "tools/steps_main.cpp"\n' > "$d/plug/mcpp.toml"
    printf 'export module plug;\nimport std;\nimport mcpp;\nexport void plug_declare() {\n    const char* p = mcpp::dep_bin("plug", "steps");\n    mcpp::warning(std::format("steps=[{}]", p ? p : "(null)").c_str());\n}\n' > "$d/plug/src/plug.cppm"
    printf 'int main() { return 0; }\n' > "$d/plug/tools/steps_main.cpp"
    printf '[package]\nname = "fw"\nversion = "0.1.0"\n\n[build-dependencies]\nplug = { path = "../plug", tools = ["steps"], host-module = true }\n\n[targets.fw]\nkind = "lib"\n' > "$d/fw/mcpp.toml"
    printf 'export module fw;\nimport mcpp;\nimport plug;\nexport void fw_declare() { plug_declare(); }\n' > "$d/fw/src/fw.cppm"
    printf '[package]\nname = "direct"\nversion = "0.1.0"\n\n[build-dependencies]\nplug = { path = "../plug", tools = ["steps"], host-module = true }\n\n[targets.direct]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/direct/mcpp.toml"
    printf 'import mcpp;\nimport plug;\nint main() { plug_declare(); return 0; }\n' > "$d/direct/build.mcpp"
    printf '[package]\nname = "through"\nversion = "0.1.0"\n\n[build-dependencies]\nfw = { path = "../fw" }\n\n[targets.through]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/through/mcpp.toml"
    printf 'import mcpp;\nimport fw;\nint main() { fw_declare(); return 0; }\n' > "$d/through/build.mcpp"
    printf 'int main() { return 0; }\n' | tee "$d/direct/src/main.cpp" > "$d/through/src/main.cpp"
    local a b
    a="$(cd "$d/direct" && run build 2>&1 | grep -o 'steps=\[[^]]*\]')"
    b="$(cd "$d/through" && run build 2>&1 | grep -o 'steps=\[[^]]*\]')"
    echo "-- the edge that names the tool: ${a:-no answer}"
    echo "-- one package away, through a framework that names it: ${b:-no answer}"
    if [ -n "$a" ] && [ "$a" != "steps=[]" ] && [ "$b" = "steps=[]" ]; then
        echo "RESULT 955: reproduced (the path reaches the declaring package only; elsewhere dep_bin answers an empty string)"
    else echo "RESULT 955: not reproduced"; fi
}

# 952s: two consecutive container jobs. A build is killed in one fresh pid
# namespace and continued in another fresh one; the namespaces' names are
# printed, because the kernel gives a later namespace an earlier one's number.
case_952s() {
    unshare -Urpf --mount-proc true 2>/dev/null || { echo "RESULT 952s: skipped: no new pid namespace here"; return; }
    local d="$ROOT/952s"; mkdir -p "$d/seed/src"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$d/seed/mcpp.toml"
    local main='import std;\n' body='int main() { int s = 0;' i
    for i in $(seq 0 23); do
        printf 'import std;\nint f%s(int x) { std::vector<std::string> v; for (int i = 0; i < x; ++i) v.push_back(std::format("{}", i)); std::ranges::sort(v); return (int)v.size(); }\n' "$i" > "$d/seed/src/u$i.cpp"
        main="$main""int f$i(int);\n"; body="$body s += f$i(3);"
    done
    printf "$main$body"' std::println("{}", s); }\n' > "$d/seed/src/main.cpp"
    printf '#!/bin/sh\nreadlink /proc/self/ns/pid > ns-a.txt\nMCPP_JOBS=%s timeout -s KILL 1.0 "$1" build >/dev/null 2>&1\nexit 0\n' "$JOBS" > "$d/kill.sh"
    printf '#!/bin/sh\nreadlink /proc/self/ns/pid > ns-b.txt\nMCPP_JOBS=%s "$1" build 2>&1 | tail -1 > result.txt\nexit 0\n' "$JOBS" > "$d/continue.sh"
    local refused=0 trials=0 seen="" t
    for t in 1 2 3 4; do
        local p="$d/t$t"; mkdir -p "$p"; cp -r "$d/seed/src" "$d/seed/mcpp.toml" "$p/"
        (cd "$p" && unshare -Urpf --mount-proc sh "$d/kill.sh" "$MCPP")
        local n=0 a; for a in "$p"/.mcpp/compiler-runs/*/run-*/attempts/*; do [ -e "$a/admitted" ] && [ ! -e "$a/retired" ] && n=$((n+1)); done
        [ "$n" -gt 0 ] || continue
        (cd "$p" && unshare -Urpf --mount-proc sh "$d/continue.sh" "$MCPP")
        trials=$((trials+1)); seen="$seen $(cat "$p/ns-b.txt")"
        echo "-- job A in $(cat "$p/ns-a.txt"), job B in $(cat "$p/ns-b.txt"): $(cut -c1-70 "$p/result.txt")"
        grep -q 'occupied or unknown' "$p/result.txt" && refused=$((refused+1))
    done
    local distinct; distinct="$(echo $seen | tr ' ' '\n' | sort -u | wc -l)"
    echo "-- $trials second jobs ran in $distinct distinct namespace names"
    [ "$trials" -gt 0 ] && [ "$refused" = "$trials" ] && echo "RESULT 952s: reproduced ($refused of $trials refused)" || echo "RESULT 952s: not reproduced ($refused of $trials refused)"
}

# 931d: what the linker itself reports. The link is given --dependency-file
# through the project's own flags; the file names what the linker read.
case_931d() {
    local d="$ROOT/931d"; mkdir -p "$d/src"; printf '{ main; };\n' > "$d/list.txt"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[build]\nldflags = ["-Wl,--dynamic-list=%s", "-Wl,--dependency-file=%s"]\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' "$d/list.txt" "$d/link.d" > "$d/mcpp.toml"
    printf 'int main() { return 0; }\n' > "$d/src/main.cpp"
    (cd "$d" && run build >/dev/null 2>&1)
    [ -f "$d/link.d" ] || { echo "RESULT 931d: not reproduced: the linker wrote no dependency file"; return; }
    echo "-- files named by the linker's dependency file: $(tr ' \\' '\n\n' < "$d/link.d" | grep -c '^/')"
    echo "-- the option's file among them: $(grep -c "$d/list.txt" "$d/link.d") line(s); a library found by search among them: $(grep -c 'libc\.so\|libc\.a\|libstdc++' "$d/link.d") line(s)"
    grep -q "$d/list.txt" "$d/link.d" && echo "RESULT 931d: reproduced (the linker names the option's file and the libraries it found)" || echo "RESULT 931d: not reproduced"
}

[ $# -gt 0 ] || set -- 960 956 952 952s 931 931d 954 958 955
for c in "$@"; do echo; echo "==== $c"; "case_$c"; done
