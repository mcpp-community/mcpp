# CMake build description for xlings — the benchmark's independent control target.
#
# WHY THIS EXISTS. mcpp measuring its own build proves nothing about build
# performance in general: an engine change can be an artefact of one project's
# module graph. xlings is written by different people against a different
# structure (110 module interface units, 46k lines, 6 dependencies), so a result
# that reproduces here is a result about the engine rather than about mcpp.
#
# THE TREE IS NOT VENDORED. A snapshot rots, and a benchmark whose target has
# drifted from the real project measures the snapshot. Point this at a checkout:
#
#   cmake -G Ninja -S bench/projects/xlings -B build-xlings \
#         -DXLINGS_ROOT=/path/to/xlings \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_CXX_COMPILER=$HOME/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++
#   cmake --build build-xlings
#
# Record the commit with the numbers; the published ones are from `b1563fe`.
#
# FAIRNESS CONTRACT — the same five as bench/projects/mcpp/CMakeLists.txt:
# same compiler binary, same language flags, same source set, same link output
# kind, same standard library (`import std;`, not a header shim).
#
# ⚠️ STATUS: CONFIGURES AND COMPILES xlings' OWN 110 UNITS; DOES NOT LINK.
#
# The blocker is not modules — it is dependency resolution, and it is worth
# stating because it is the honest limit of a hand-written foreign description:
#
#   xlings declares 6 direct dependencies. Four ship source (mcpplibs cmdline /
#   xpkg / tinyhttps / capi.lua) and two ship source trees that mcpp builds
#   (ftxui, libarchive). Wiring those four in surfaced THEIR dependencies:
#
#     tinyhttps/src/tls.cppm:3    fatal error: mbedtls/ssl.h: No such file
#     capi.lua/.../lua_headers.h  fatal error: lua.h: No such file
#
#   Following that graph to a link means reimplementing mcpp's package manager
#   inside a CMakeLists — and a hand-written version of it is stale on the first
#   upstream change, silently.
#
# So the xlings arm compares **mcpp against mcpp** (releases, schedules), which
# is what a control target is for: it answers "does this engine change hold on a
# codebase nobody tuned it for?", and that question needs no second engine.
# bench/projects/mcpp/ keeps the cross-engine arm, because mcpp has one source
# dependency and this repository can keep that description correct.

cmake_minimum_required(VERSION 3.30)

# `import std;` is still behind an experimental gate whose key changes with the
# CMake version — this is the CMake 4.0 key. Must be set BEFORE project().
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")

project(xlings CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD 1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Where the tree is.
# ---------------------------------------------------------------------------
if(NOT XLINGS_ROOT AND DEFINED ENV{XLINGS_ROOT})
  set(XLINGS_ROOT "$ENV{XLINGS_ROOT}")
endif()
if(NOT XLINGS_ROOT OR NOT EXISTS "${XLINGS_ROOT}/mcpp.toml")
  message(FATAL_ERROR
    "set -DXLINGS_ROOT=<checkout of https://github.com/openxlings/xlings>; "
    "this description is deliberately not vendored — see README.md")
endif()

# ---------------------------------------------------------------------------
# The hermetic payload, exactly as bench/projects/mcpp does it.
#
# CMAKE_CXX_FLAGS, not add_compile_options(): CMake generates the `std` module
# target ITSELF and directory-scope options do not reach it. Without this the
# std module compiles against the compiler's default libc headers while every
# xlings unit compiles against the sysroot, and the build dies on a type that
# exists in both (`conflicting type for imported declaration '_IO_FILE'`) — an
# error that names neither the flag nor the target that is wrong.
# ---------------------------------------------------------------------------
if(DEFINED ENV{MCPP_HOME})
  set(MCPP_HOME "$ENV{MCPP_HOME}")
else()
  set(MCPP_HOME "$ENV{HOME}/.mcpp")
endif()
set(MCPP_XPKGS "${MCPP_HOME}/registry/data/xpkgs")

file(GLOB MCPP_BINUTILS_DIRS "${MCPP_XPKGS}/xim-x-binutils/*")
if(MCPP_BINUTILS_DIRS)
  list(SORT MCPP_BINUTILS_DIRS)
  list(GET MCPP_BINUTILS_DIRS -1 MCPP_BINUTILS)
  string(APPEND CMAKE_CXX_FLAGS " -B${MCPP_BINUTILS}/bin")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -B${MCPP_BINUTILS}/bin")
endif()
if(IS_DIRECTORY "${MCPP_HOME}/registry/subos/default")
  string(APPEND CMAKE_CXX_FLAGS " --sysroot=${MCPP_HOME}/registry/subos/default")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " --sysroot=${MCPP_HOME}/registry/subos/default")
endif()

# ---------------------------------------------------------------------------
# Source set — xlings' mcpp.toml infers `src/**/*.{cppm,cpp}` and names
# src/main.cpp as the binary's entry point.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE XLINGS_MODULES CONFIGURE_DEPENDS "${XLINGS_ROOT}/src/*.cppm")
list(LENGTH XLINGS_MODULES XLINGS_MODULE_COUNT)
if(XLINGS_MODULE_COUNT EQUAL 0)
  message(FATAL_ERROR "no module interface units under ${XLINGS_ROOT}/src")
endif()

add_executable(xlings "${XLINGS_ROOT}/src/main.cpp")
target_sources(xlings
  PRIVATE FILE_SET CXX_MODULES BASE_DIRS "${XLINGS_ROOT}/src" FILES ${XLINGS_MODULES})

# `[build] include_dirs = ["src/libs/json"]` — src/libs/json.cppm reaches for
# <json.hpp> from its global module fragment.
target_include_directories(xlings PRIVATE "${XLINGS_ROOT}/src/libs/json")
# `[build] cxxflags`
target_compile_definitions(xlings PRIVATE LIBARCHIVE_STATIC UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# Dependencies.
#
# The four mcpplibs packages ship SOURCE and are compiled from it — each needs
# its own FILE_SET, because a CXX_MODULES set requires every file to live under
# one of its base directories and these sit in the registry outside the tree.
#
# Versions are PINNED to xlings' mcpp.toml. Newer ones are usually also unpacked
# in the registry, and taking the newest would mean the two arms compile
# different code — a benchmark whose fairness rests on directory ordering is not
# a benchmark.
#
# mcpp stages prebuilt objects for these out of its global cache while cmake
# compiles them from source: a handicap on cmake's cold build, declared here
# rather than hidden.
# ---------------------------------------------------------------------------
function(xlings_add_source_dep name version)
  set(dir "${MCPP_XPKGS}/${name}/${version}")
  file(GLOB_RECURSE srcs CONFIGURE_DEPENDS "${dir}/*/src/*.cppm")
  if(NOT srcs)
    message(WARNING "dependency ${name} ${version} not unpacked at ${dir}; "
                    "this build will not match mcpp's own")
    return()
  endif()
  list(GET srcs 0 first)
  get_filename_component(base "${first}" DIRECTORY)
  # A file-set name may only contain letters, digits and underscores — package
  # names like `mcpplibs.capi-x-lua` do not qualify, and CMake rejects them at
  # configure time rather than mangling them.
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" fsname "fs_${name}")
  target_sources(xlings PRIVATE
    FILE_SET "${fsname}" TYPE CXX_MODULES BASE_DIRS "${base}" FILES ${srcs})
endfunction()

xlings_add_source_dep(mcpplibs-x-cmdline   0.0.2)
xlings_add_source_dep(mcpplibs-x-xpkg      0.0.57)
xlings_add_source_dep(mcpplibs-x-tinyhttps 0.2.9)
xlings_add_source_dep(mcpplibs.capi-x-lua  0.0.3)

# ftxui and libarchive arrive as SOURCE trees, unpacked one level below the
# version directory (`compat-x-ftxui/6.1.9/FTXUI-6.1.9/include`,
# `compat-x-libarchive/3.8.7/libarchive-3.8.7/libarchive`) — not as prebuilt
# libraries. Globbing `<ver>/include` finds nothing and the first importer dies
# with `fatal error: ftxui/component/event.hpp: No such file or directory`.
#
# ⚠️ THIS ARM IS INCOMPLETE, and that is stated rather than papered over: mcpp
# BUILDS these two from source through its own package machinery, and
# reproducing that here means compiling ftxui and libarchive with cmake as well.
# Until that is done, this description configures and compiles xlings' own units
# but cannot LINK. The headers are wired so the compile phase — which is what
# the module-graph benchmark measures — is comparable.
foreach(pkg IN ITEMS compat-x-ftxui compat-x-libarchive)
  file(GLOB pkgdirs "${MCPP_XPKGS}/${pkg}/*")
  foreach(pkgdir IN LISTS pkgdirs)
    file(GLOB inner "${pkgdir}/*")
    foreach(d IN LISTS inner)
      if(IS_DIRECTORY "${d}/include")
        target_include_directories(xlings PRIVATE "${d}/include")
      endif()
      if(IS_DIRECTORY "${d}/libarchive")
        target_include_directories(xlings PRIVATE "${d}/libarchive")
      endif()
    endforeach()
  endforeach()
endforeach()

target_link_options(xlings PRIVATE -static-libstdc++)

message(STATUS "xlings: ${XLINGS_MODULE_COUNT} module interface units from ${XLINGS_ROOT}")
