#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): `import std` on i686-windows-msvc with
# clang 23.1.3 and 22.1.8, mcpp built from the #781 head. Answers G1/G2 of
# .agents/reviews/2026-10-08-llvm-2313-part3-review-fixes-and-x86-msvc-coroutines-design.md.
# Prints; asserts nothing.
set -u
M="${MCPP_BUILT:?}"
W="$RUNNER_TEMP/x86-msvc-std"; mkdir -p "$W"
"$M" --version

# case <name> <target> <standard> <toolchain-table> <source-kind>
case_run() {
  local name=$1 target=$2 std=$3 tc=$4 src=$5
  local d="$W/$name"; rm -rf "$d"; mkdir -p "$d/src"; cd "$d"
  printf '[package]\nname = "p"\nversion = "0.1.0"\nstandard = "%s"\n\n%s\n' "$std" "$tc" > mcpp.toml
  case "$src" in
    std)     printf 'import std;\nint main() { std::println("std ok {}", sizeof(void*)); }\n' > src/main.cpp ;;
    compat)  printf 'import std.compat;\nint main() { std::printf("compat ok %%zu\\n", sizeof(void*)); }\n' > src/main.cpp ;;
    std20)   printf 'import std;\nint main() { std::cout << "std ok " << sizeof(void*) << "\\n"; }\n' > src/main.cpp ;;
    coro)    printf '#include <coroutine>\nstruct t { struct promise_type { t get_return_object() { return {}; } std::suspend_never initial_suspend() noexcept { return {}; } std::suspend_never final_suspend() noexcept { return {}; } void return_void() {} void unhandled_exception() {} }; };\nt f() { co_return; }\nint main() { f(); }\n' > src/main.cpp ;;
  esac
  echo; echo "################ $name  target=$target standard=$std src=$src"
  echo "--- mcpp.toml"; cat mcpp.toml
  echo "--- build"
  "$M" build --verbose --target "$target" > build.log 2>&1; local rc=$?
  cat build.log; echo "build rc=$rc"
  echo "--- lines naming generator / coroutine / __cpp_impl_coroutine:"
  grep -nE 'generator|coroutine|__cpp_impl_coroutine' build.log | head -30
  if [ $rc -eq 0 ]; then
    local exe; exe=$(find target -type f -name 'p.exe' | head -1)
    echo "--- run $exe"; "$exe"; echo "run rc=$?"
    command -v file >/dev/null && file "$exe"
  fi
}

T23='[toolchain]
windows = "llvm@23.1.3"'
T22i='[toolchain]
windows = "llvm@23.1.3"

[target.i686-windows-msvc]
toolchain = "llvm@22.1.8"'
T22x='[toolchain]
windows = "llvm@23.1.3"

[target.x86-windows-msvc]
toolchain = "llvm@22.1.8"'

# G1: the reported failure, and what the compiler says where.
case_run g1-23-cxx23-i686        i686-windows-msvc   c++23 "$T23"  std
# G2: C++20 on the same compiler and target.
case_run g2-23-cxx20-i686        i686-windows-msvc   c++20 "$T23"  std20
case_run g2-23-cxx20-i686-compat i686-windows-msvc   c++20 "$T23"  compat
# Using the coroutine library itself under C++20 (expected to be refused by the
# STL or the compiler; recorded for the documentation).
case_run g2-23-cxx20-i686-coro   i686-windows-msvc   c++20 "$T23"  coro
# Option (b): llvm 22.1.8 for this target only, in the spelling the note uses
# and in MSVC's x86 spelling.
case_run g1b-22-cxx23-i686       i686-windows-msvc   c++23 "$T22i" std
case_run g1b-22-cxx23-x86        x86-windows-msvc    c++23 "$T22x" std
# G3: the 64-bit row is unaffected.
case_run g3-23-cxx23-x64         x86_64-windows-msvc c++23 "$T23"  std
