#!/usr/bin/env bash
# Reproductions for .agents/docs/2026-10-10-one-source-one-meaning-design.md.
# Prints what each case observes; asserts nothing.
#
#   MCPP=<path to mcpp> bash 2026-10-10-one-source-one-meaning-repro.sh
#
# R1 conditional dialect_cxxflags in three positions (#786, #785)
# R2 action output shared by two profiles (out_dir has no configuration)
# R3 #778 on Linux: member target cleared, workspace ninja log kept
# R4 #790: implementation partition in a .cpp, rebuilt every time
# R5 interface unit (`export module`) in a .cpp
# R6 implementation partition in a .cpp imported by the interface
set -u
MCPP="${MCPP:-mcpp}"
LLVM="${LLVM:-llvm@23.1.3}"
T=$(mktemp -d)
echo "mcpp: $("$MCPP" --version 2>&1 | head -1)   work: $T"
binrun() { local b; b=$(find target -path '*/bin/*' -name "$1" -type f -newer "$2" 2>/dev/null | xargs -r ls -t | head -1); [ -n "$b" ] && "$b" || echo "(no binary)"; }

# ── R1 ────────────────────────────────────────────────────────────────
main_dial() { cat > "$1" <<'EOF'
#include <cstdio>
#ifndef DIAL
#define DIAL 0
#endif
int main() { std::printf("DIAL=%d\n", DIAL); }
EOF
}
app_toml() { cat <<EOF
[package]
name = "app"
version = "0.1.0"
[build]
sources = ["main.cpp"]
[targets.app]
kind = "bin"
main = "main.cpp"
$1
EOF
}
COND1='[target.'"'"'cfg(os = "linux")'"'"'.build]
dialect_cxxflags = ["-DDIAL=1"]'
COND2='[target.'"'"'cfg(os = "linux")'"'"'.build]
dialect_cxxflags = ["-DDIAL=2"]'
mkdir -p "$T/r1/ws-root/app" "$T/r1/member/app" "$T/r1/standalone"
printf '[workspace]\nmembers = ["app"]\n%s\n' "$COND1" > "$T/r1/ws-root/mcpp.toml"
app_toml "" > "$T/r1/ws-root/app/mcpp.toml"; main_dial "$T/r1/ws-root/app/main.cpp"
printf '[workspace]\nmembers = ["app"]\n' > "$T/r1/member/mcpp.toml"
app_toml "$COND2" > "$T/r1/member/app/mcpp.toml"; main_dial "$T/r1/member/app/main.cpp"
app_toml "$COND2" > "$T/r1/standalone/mcpp.toml"; main_dial "$T/r1/standalone/main.cpp"
for c in ws-root member; do
    (cd "$T/r1/$c" && "$MCPP" build -p app > build.log 2>&1; echo "R1 $c: $(binrun app mcpp.toml)  warnings: $(grep -ci warn build.log)")
done
(cd "$T/r1/standalone" && "$MCPP" build > build.log 2>&1; echo "R1 standalone: $(binrun app mcpp.toml)")

# ── R2 ────────────────────────────────────────────────────────────────
mkdir -p "$T/r2/src"; cd "$T/r2"
cat > mcpp.toml <<'EOF'
[package]
name = "gp"
version = "0.1.0"
[build]
sources = ["src/main.cpp"]
[targets.gp]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
const char* gen_profile();
int main() { std::printf("GEN=%s\n", gen_profile()); }
EOF
printf '#!/bin/sh\nprintf '"'"'const char* gen_profile() { return "%%s"; }\\n'"'"' "$1" > "$2"\n' > gen.sh
cat > build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = (std::filesystem::path(mcpp::out_dir()) / "gen.cpp").generic_string();
    const auto gen = (root / "gen.sh").generic_string();
    mcpp::action a;
    a.id = "gen";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(mcpp::profile()).arg(out.c_str())
        .input(gen.c_str()).output(out.c_str()).submit();
    return 0;
}
EOF
for p in "" --release ""; do
    "$MCPP" build $p > build.log 2>&1 || tail -5 build.log
    echo "R2 build ${p:-(dev)}: $(binrun gp gen.sh)"
done
echo "R2 out_dir: $(find target -name gen.cpp | head -3 | tr '\n' ' ')"

# ── R3 ────────────────────────────────────────────────────────────────
mkdir -p "$T/r3/app"; cd "$T/r3"
printf '[workspace]\nmembers = ["app"]\n' > mcpp.toml
cat > app/mcpp.toml <<'EOF'
[package]
namespace = "repro"
name = "app"
version = "0.0.1"
[build]
sources = []
[targets.resource_test]
kind = "bin"
main = "main.cpp"
EOF
printf 'extern const char* resource();\nint main() { return resource()[0] == '"'"'R'"'"' ? 0 : 1; }\n' > app/main.cpp
printf '#!/bin/sh\nprintf '"'"'const char* resource() { return "RESOURCE"; }\\n'"'"' > "$1"\n' > app/gen.sh
cat > app/build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const auto root = std::filesystem::path(mcpp::manifest_dir());
    const auto out = (std::filesystem::path(mcpp::out_dir()) / "generated.cpp").generic_string();
    const auto gen = (root / "gen.sh").generic_string();
    mcpp::action a;
    a.id = "generate:resource";
    a.role = mcpp::roles::source;
    a.arg("sh").arg(gen.c_str()).arg(out.c_str()).input(gen.c_str()).output(out.c_str()).submit();
    return 0;
}
EOF
"$MCPP" build -p app --release > b1.log 2>&1; echo "R3 first: rc=$? size=$(stat -c %s app/target/.build-mcpp/out/generated.cpp 2>/dev/null)"
mv app/target saved-app-target
"$MCPP" build -p app --release > b2.log 2>&1; echo "R3 after clearing member target: rc=$? size=$(stat -c %s app/target/.build-mcpp/out/generated.cpp 2>/dev/null) $(grep -m1 -o 'undefined reference[^)]*' b2.log)"

# ── R4–R6 ─────────────────────────────────────────────────────────────
mod_pkg() { # dir, extra-for-repro.cppm
mkdir -p "$1/src"
cat > "$1/mcpp.toml" <<EOF
[package]
name = "repro"
version = "0.1.0"
[toolchain]
default = "$LLVM"
[targets.repro]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'export module repro;\nexport import :api;\n%s\n' "$2" > "$1/src/repro.cppm"
printf 'export module repro:api;\nexport auto answer() -> int;\n' > "$1/src/api.cppm"
printf 'module repro:api_impl;\nimport :api;\nauto answer() -> int { return 42; }\n' > "$1/src/api_impl.cpp"
printf 'import repro;\nimport std;\nauto main() -> int { std::println("{}", answer()); }\n' > "$1/src/main.cpp"
}
mod_pkg "$T/r4" ""; cd "$T/r4"
for i in 1 2 3; do "$MCPP" build --verbose > b$i.log 2>&1; done
echo "R4 build 2: $(grep -cE '^\[[0-9]+/' b2.log) steps; build 3: $(grep -cE '^\[[0-9]+/' b3.log) steps"
echo "R4 pcm files: $(find target -name '*.pcm' -path '*pcm.cache*' -printf '%f ' 2>/dev/null)"

mod_pkg "$T/r6" "import :api_impl;"; cd "$T/r6"
"$MCPP" build > b.log 2>&1; echo "R6 import of the .cpp partition: rc=$? $(grep -m1 -o "fatal error: .*" b.log)"

mkdir -p "$T/r5/src"; cd "$T/r5"
cat > mcpp.toml <<EOF
[package]
name = "r5"
version = "0.1.0"
[toolchain]
default = "$LLVM"
[targets.r5]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'export module r5;\nexport auto answer() -> int { return 42; }\n' > src/r5.cpp
printf 'import r5;\nimport std;\nauto main() -> int { std::println("{}", answer()); }\n' > src/main.cpp
"$MCPP" build > b1.log 2>&1; r1=$?; "$MCPP" build --verbose > b2.log 2>&1
echo "R5 interface in .cpp: rc=$r1 $(grep -m1 -oE '(error|fatal error): .*' b1.log) second build: $(grep -cE '^\[[0-9]+/' b2.log) steps"
