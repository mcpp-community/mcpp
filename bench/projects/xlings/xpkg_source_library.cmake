# Build a registry package from its OWN manifest, for the cmake arm.
#
# WHY THIS EXISTS. xlings links ftxui, libarchive, lua and mbedtls, and mcpp's
# registry ships all four as SOURCE — there is no prebuilt .a to point at. The
# cmake arm therefore has to compile them, and the first two attempts at that
# both failed:
#
#   * add_subdirectory() on the vendored CMakeLists. libarchive builds its test
#     suite unconditionally enough that configure dies in
#     `FILE STRINGS ... test_read_format_cab_skip_malformed.c cannot be read`
#     (its own switch is ENABLE_TEST, singular — BUILD_TESTING/ENABLE_TESTING
#     are ignored), and mbedtls 3.6.1 has an UNCONDITIONAL FATAL_ERROR at
#     CMakeLists.txt:304 when `framework/CMakeLists.txt` is absent. The registry
#     tarball has no `framework/` submodule, so that path can never work — it is
#     not gated by any option. lua ships no CMakeLists at all.
#
#   * A glob of the unpacked tree. `libarchive/*.c` is 132 files where mcpp
#     compiles 127, and `lua/src/*.c` is 34 where mcpp compiles 32 — the two
#     extra being lua.c and luac.c, the interpreter and the bytecode compiler,
#     each with its own main(). Linking either into xlings is a duplicate-symbol
#     error, and the five extra libarchive files are the ones its own configure
#     decides against.
#
# So the source set is READ OUT OF THE MANIFEST. Every package in mcpp's
# registry carries `.xpkg.lua` beside its unpacked tree, and its `mcpp = { … }`
# table is the exact `sources` / `include_dirs` / `cflags` mcpp itself compiles
# the package with. Reading it is what makes this a fair arm: both engines then
# compile the same 127 files with the same defines. A copied list would be a
# fifth place the same decision lives, and bench/projects/xlings/README.md
# already records what happened the last time this repo copied a generated list
# (embed_lua_stdlib.cmake, which dropped one of eleven modules and surfaced the
# loss three files away, in a consumer).
#
# WHAT THIS DOES NOT DO. It is not a Lua interpreter — it reads four flat
# string-list fields out of one table whose shape every Form B manifest shares.
# Anything it cannot find is a FATAL_ERROR rather than an empty list, because
# the failure mode being avoided here is precisely "compiled a subset and
# reported a build time for it".

# ---------------------------------------------------------------------------
# Manifest reading
# ---------------------------------------------------------------------------

# The text of a package's `mcpp = { … }` table, and of the sub-table for the
# platform being built, if it has one.
function(_bench_xpkg_manifest verdir out_main out_plat)
  set(manifest "${verdir}/.xpkg.lua")
  if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR
      "bench: no .xpkg.lua at ${verdir}. That file is what says which sources "
      "mcpp compiles this package from; without it this arm would guess.")
  endif()
  file(READ "${manifest}" text)

  # `\n    mcpp = {` — matching the four-space indent is what tells the
  # package-level table apart from one NAMED IN A COMMENT at column 0, which
  # mcpplibs-x-cmdline's manifest opens with (`-- Form B (inline mcpp = {…})`).
  string(FIND "${text}" "\n    mcpp = {" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR
      "bench: ${manifest} has no `mcpp = {` table (a Form A descriptor, which "
      "defers to an upstream mcpp.toml). This helper only reads Form B.")
  endif()
  string(SUBSTRING "${text}" ${pos} -1 seg)

  # Platform sub-tables (`windows = { cxxflags = … }`) sit at the end of the
  # table and MUST be cut off the main one, not just skipped: ftxui has no
  # top-level cxxflags and a windows one, so a search over the whole table
  # finds the Windows flags and applies -DUNICODE on Linux. The current
  # platform's table is taken out first, by name.
  if(WIN32)
    set(plat_key "windows")
  elseif(APPLE)
    set(plat_key "macosx")
  else()
    set(plat_key "linux")
  endif()
  set(plat "")
  string(FIND "${seg}" "\n        ${plat_key} = {" ppos)
  if(NOT ppos EQUAL -1)
    string(SUBSTRING "${seg}" ${ppos} -1 plat)
  endif()
  set(cut -1)
  foreach(k linux macosx windows)
    string(FIND "${seg}" "\n        ${k} = {" p)
    if(NOT p EQUAL -1)
      if(cut EQUAL -1 OR p LESS cut)
        set(cut ${p})
      endif()
    endif()
  endforeach()
  if(NOT cut EQUAL -1)
    string(SUBSTRING "${seg}" 0 ${cut} seg)
  endif()

  set(${out_main} "${seg}"  PARENT_SCOPE)
  set(${out_plat} "${plat}" PARENT_SCOPE)
endfunction()

# One `field = { "a", "b" }` list out of a manifest table, as a CMake list.
# Missing field -> empty, which the callers check where emptiness is wrong.
function(_bench_xpkg_field seg field out)
  set(result "")
  if("${seg}" MATCHES "[\r\n][ \t]*${field}[ \t]*=[ \t]*{([^}]*)}")
    # Lua string literals, escapes included: "([^"\]|\.)*"
    string(REGEX MATCHALL "\"([^\"\\\\]|\\\\.)*\"" quoted "${CMAKE_MATCH_1}")
    foreach(q IN LISTS quoted)
      string(REGEX REPLACE "^\"" "" v "${q}")
      string(REGEX REPLACE "\"$" "" v "${v}")
      # TWO un-escaping passes, and both are real. The manifest holds a Lua
      # string whose VALUE is a shell token: libarchive's
      #     "-DPLATFORM_CONFIG_H=\\\"mcpp_libarchive_config.h\\\""
      # is the Lua spelling of  -DPLATFORM_CONFIG_H=\"…\"  which mcpp splices
      # into a command line, where the shell strips the backslashes and the
      # macro ends up as the quoted string `#include PLATFORM_CONFIG_H` needs.
      # CMake does its own shell-escaping of compile options, so what it must
      # be handed is the SHELL-LEVEL value — one pass short and the macro
      # expands to \"mcpp_libarchive_config.h\", which fails as
      # `#include` with a stray backslash rather than as a bad flag.
      string(REGEX REPLACE "\\\\(.)" "\\1" v "${v}")
      string(REGEX REPLACE "\\\\(.)" "\\1" v "${v}")
      list(APPEND result "${v}")
    endforeach()
  endif()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# One `field = "value"` scalar (c_standard, language) out of a manifest table.
function(_bench_xpkg_scalar seg field out)
  set(result "")
  if("${seg}" MATCHES "[\r\n][ \t]*${field}[ \t]*=[ \t]*\"([^\"]*)\"")
    set(result "${CMAKE_MATCH_1}")
  endif()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Path patterns
# ---------------------------------------------------------------------------
# Manifest paths are relative to the VERSION directory, and a leading `*`
# absorbs the tarball's wrap layer (`compat-x-lua/5.4.7/lua-5.4.7/…`). A bare
# path is the version directory itself — that is where `mcpp_generated/` holds
# the config headers mcpp materialises from the manifest's `generated_files`
# (mcpp_libarchive_config.h, mcpp_lua_platform_config.h, mcpp_zlib_config.h).
# `!` prefixes an exclusion (ftxui's *_test.cpp / *_fuzzer.cpp, zstd's
# zstd_trace.c).

function(_bench_xpkg_expand verdir patterns out)
  set(files "")
  foreach(p IN LISTS patterns)
    if(p MATCHES "\\*\\*")
      # CMake has no `**`: GLOB_RECURSE already recurses below the matched
      # directory, so `a/**/*.cpp` is spelled `a/*.cpp` there.
      string(REPLACE "/**/" "/" p "${p}")
      file(GLOB_RECURSE hit "${verdir}/${p}")
      list(APPEND files ${hit})
    else()
      file(GLOB hit "${verdir}/${p}")
      list(APPEND files ${hit})
    endif()
  endforeach()
  if(files)
    list(REMOVE_DUPLICATES files)
  endif()
  set(${out} "${files}" PARENT_SCOPE)
endfunction()

# Resolve a manifest pattern list against the unpacked tree.
#
# A pattern that matches NOTHING is fatal. The whole point of reading the
# manifest is that the arms compile the same files; a pattern that quietly
# resolves to zero would put this arm back where the glob was, one translation
# unit short and reporting a time for it.
function(_bench_xpkg_resolve verdir what patterns out)
  set(keep "")
  set(drop "")
  foreach(p IN LISTS patterns)
    if(p MATCHES "^!(.+)$")
      list(APPEND drop "${CMAKE_MATCH_1}")
    else()
      _bench_xpkg_expand("${verdir}" "${p}" hit)
      if(NOT hit)
        message(FATAL_ERROR
          "bench: ${what} pattern '${p}' matched nothing under ${verdir}. The "
          "package is unpacked but its manifest and its tree disagree.")
      endif()
      list(APPEND keep ${hit})
    endif()
  endforeach()
  _bench_xpkg_expand("${verdir}" "${drop}" dropped)
  if(dropped)
    list(REMOVE_ITEM keep ${dropped})
  endif()
  list(REMOVE_DUPLICATES keep)
  list(SORT keep)
  set(${out} "${keep}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The target
# ---------------------------------------------------------------------------
# STATIC, because that is the link kind mcpp produces for a `kind = "lib"`
# package and the fairness contract in CMakeLists.txt covers the output kind.
#
# The VERSION IS PASSED IN, pinned by the caller from xlings' resolved
# dependency set. The registry holds several versions of some packages and
# "newest wins" would have the two arms compile different code.
function(bench_add_xpkg_library target pkg version)
  bench_registry_xpkgs(xpkgs)
  set(verdir "${xpkgs}/${pkg}/${version}")
  if(NOT IS_DIRECTORY "${verdir}")
    message(FATAL_ERROR
      "bench: ${pkg} ${version} is not unpacked at ${verdir}. Run the mcpp arm "
      "once (or `mcpp build` in the xlings tree) to populate the registry.")
  endif()

  _bench_xpkg_manifest("${verdir}" seg plat)
  _bench_xpkg_field("${seg}" sources      pat_srcs)
  _bench_xpkg_field("${seg}" include_dirs pat_incs)
  _bench_xpkg_field("${seg}" cflags       cflags)
  _bench_xpkg_field("${seg}" cxxflags     cxxflags)
  _bench_xpkg_scalar("${seg}" c_standard  c_standard)
  if(plat)
    _bench_xpkg_field("${plat}" cflags   plat_cflags)
    _bench_xpkg_field("${plat}" cxxflags plat_cxxflags)
    list(APPEND cflags   ${plat_cflags})
    list(APPEND cxxflags ${plat_cxxflags})
  endif()

  if(NOT pat_srcs)
    message(FATAL_ERROR "bench: ${pkg}'s manifest has no `sources` list")
  endif()
  _bench_xpkg_resolve("${verdir}" "${pkg} sources" "${pat_srcs}" srcs)

  # include_dirs are globs too, and `"*"` (libarchive, zlib) resolves to
  # everything beside the tree — tarballs included. Directories only.
  _bench_xpkg_expand("${verdir}" "${pat_incs}" inc_hits)
  set(incs "")
  foreach(d IN LISTS inc_hits)
    if(IS_DIRECTORY "${d}")
      list(APPEND incs "${d}")
    endif()
  endforeach()

  add_library(${target} STATIC ${srcs})

  # PUBLIC: mcpp propagates a package's include dirs to whatever imports it,
  # and xlings' own units reach for <archive.h>, <lua.h>, <mbedtls/ssl.h> and
  # <ftxui/…> directly. This is what replaced a hand-written list of registry
  # subdirectories in CMakeLists.txt.
  target_include_directories(${target} PUBLIC ${incs})

  # PRIVATE: these are how the package compiles ITSELF (`-include
  # mcpp_zlib_config.h`, `-DZSTD_DISABLE_ASM=1`). The defines a CONSUMER needs
  # — LIBARCHIVE_STATIC — are in xlings' own mcpp.toml and set on that target.
  #
  # Split on spaces first: `-include mcpp_zlib_config.h` is one manifest string
  # but two argv words, and passed whole it reaches the compiler as a single
  # quoted argument that gcc reports as an unrecognised option.
  foreach(f IN LISTS cflags)
    string(REPLACE " " ";" toks "${f}")
    foreach(t IN LISTS toks)
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${t}>")
    endforeach()
  endforeach()
  foreach(f IN LISTS cxxflags)
    string(REPLACE " " ";" toks "${f}")
    foreach(t IN LISTS toks)
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${t}>")
    endforeach()
  endforeach()

  if(c_standard AND c_standard MATCHES "c([0-9]+)")
    # mcpp emits `-std=<c_standard>` verbatim, so extensions OFF — `gnu11` and
    # `c11` are not the same dialect and these packages were configured for the
    # strict one (which is why their manifests define _GNU_SOURCE by hand).
    set_target_properties(${target} PROPERTIES
      C_STANDARD ${CMAKE_MATCH_1} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  endif()

  # None of these packages is a module (`import_std = false`, no `modules`
  # key), and scanning them for module dependencies would add a scan of ~400
  # translation units to every cold build that mcpp does not pay.
  set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

  list(LENGTH srcs n)
  message(STATUS "bench: ${pkg} ${version} — ${n} sources from .xpkg.lua")
endfunction()
