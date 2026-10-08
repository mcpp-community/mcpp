#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): how the machine's MSVC STL guards its
# coroutine headers, and which headers of the std module depend on them.
# Prints; asserts nothing.
set -u
vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
vs=$("$vswhere" -latest -products '*' -property installationPath | tr -d '\r')
vs=$(cygpath -u "$vs")
for tools in "$vs"/VC/Tools/MSVC/*; do
  inc="$tools/include"; mods="$tools/modules"
  echo "### toolset $(basename "$tools")"
  echo "--- x86 libraries present:"; ls -d "$tools/lib/x86" 2>&1
  echo "--- <coroutine> preprocessor lines:"
  grep -nE '^\s*#\s*(if|ifdef|ifndef|elif|else|endif|error|define _COROUTINE_)|_EMIT_STL|__cpp_impl_coroutine' "$inc/coroutine" | head -40
  echo "--- <generator> preprocessor lines:"
  grep -nE '^\s*#\s*(if|ifdef|ifndef|elif|else|endif|error|define _GENERATOR_)|_EMIT_STL|__cpp_impl_coroutine|__cpp_lib_coroutine|include <coroutine>' "$inc/generator" | head -40
  echo "--- yvals_core.h coroutine feature lines:"
  grep -nE '__cpp_impl_coroutine|__cpp_lib_coroutine|__cpp_lib_generator' "$inc/yvals_core.h"
  echo "--- std.ixx lines naming generator/coroutine (with the surrounding conditionals):"
  grep -nE 'generator|coroutine|^\s*#\s*(if|elif|else|endif)' "$mods/std.ixx" | head -60
  echo "--- STL headers that use coroutine_handle / suspend_always:"
  grep -lE 'coroutine_handle|suspend_always' "$inc"/* 2>/dev/null | xargs -n1 basename
done
