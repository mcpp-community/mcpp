# CMake build description for mcpp — a like-for-like counterpart to mcpp.toml
# and to the xmake.lua beside it.
#
# WHY THIS FILE EXISTS. The build-engine benchmark in bench/ uses cmake as its
# performance baseline, and until now cmake could only build the synthetic
# fixture. The interesting workload is mcpp itself: 139 module interface units,
# 57k lines, every one of them `import std;`. Without this file the real-project
# arm had no baseline to be measured against.
#
# FAIRNESS CONTRACT — all five must hold or the comparison means nothing:
#   1. same compiler binary   — the harness passes -DCMAKE_CXX_COMPILER, and the
#                               payload's binutils + sysroot are added below
#   2. same language flags    — -std=c++23, -O2 in release
#   3. same source set        — src/**.cppm + src/main.cpp + the pinned
#                               mcpplibs.cmdline units
#   4. same link output kind  — one binary, -static-libstdc++
#   5. same standard library  — `import std;`, not a header shim
#
# Usage (benchmark):
#   cmake -G Ninja -S bench/projects/mcpp -B build-cmake \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_CXX_COMPILER=$HOME/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++
#   cmake --build build-cmake

cmake_minimum_required(VERSION 3.30)

# `import std;` is still behind an experimental gate whose key changes with the
# CMake version — this is the CMake 4.0 key. Set BEFORE project(), because the
# compiler-support probe that reads it runs during project().
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")

project(mcpp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Every mcpp module says `import std;`. This asks CMake to build the standard
# library module from the compiler's own libstdc++.modules.json, which the
# hermetic gcc payload ships.
set(CMAKE_CXX_MODULE_STD 1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# The hermetic payload.
#
# mcpp always passes an explicit -B<binutils> and --sysroot; a bare g++ from the
# payload otherwise falls back to PATH for `as`/`ld` and picks up whatever shim
# is there — on a machine with xlings installed, a stale one. The two arms must
# drive an identical process tree, so reproduce the full triple here rather than
# hoping the environment matches.
#
# -B and --sysroot must reach BOTH compile and link: the driver spawns `as` from
# it at compile time and `ld` from it at link time. Adding it on one side only
# silently falls through to PATH.
# ---------------------------------------------------------------------------
if(DEFINED ENV{MCPP_HOME})
  set(MCPP_HOME "$ENV{MCPP_HOME}")
else()
  set(MCPP_HOME "$ENV{HOME}/.mcpp")
endif()
# This file lives in bench/projects/mcpp/, so the tree it builds is three up.
# Resolved to an absolute path once, because a FILE_SET's base directory and a
# relative glob disagree about what "here" means.
get_filename_component(MCPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT EXISTS "${MCPP_ROOT}/mcpp.toml")
  message(FATAL_ERROR "expected mcpp's tree at ${MCPP_ROOT} (no mcpp.toml there)")
endif()

set(MCPP_XPKGS "${MCPP_HOME}/registry/data/xpkgs")
set(MCPP_SYSROOT "${MCPP_HOME}/registry/subos/default")

file(GLOB MCPP_BINUTILS_DIRS "${MCPP_XPKGS}/xim-x-binutils/*")
# CMAKE_CXX_FLAGS, not add_compile_options(): CMake generates the `std` module
# target ITSELF, and directory-scope options do not reach it. Without this the
# std module compiles against whatever libc headers the compiler defaults to
# while every mcpp unit compiles against the sysroot, and the build dies on a
# type that exists in both:
#
#   error: conflicting type for imported declaration 'char _IO_FILE::_unused2 [20]'
#     .../xlings/.../glibc-2.39/include/bits/types/struct_FILE.h:98
#     note: existing declaration 'char _IO_FILE::_unused2 [8]'
#     .../mcpp/registry/subos/default/usr/include/bits/types/struct_FILE.h:109
#
# Two glibcs in one link, and the error names neither the flag nor the target
# that is wrong.
if(MCPP_BINUTILS_DIRS)
  list(SORT MCPP_BINUTILS_DIRS)
  list(GET MCPP_BINUTILS_DIRS -1 MCPP_BINUTILS)
  string(APPEND CMAKE_CXX_FLAGS " -B${MCPP_BINUTILS}/bin")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -B${MCPP_BINUTILS}/bin")
endif()
if(IS_DIRECTORY "${MCPP_SYSROOT}")
  string(APPEND CMAKE_CXX_FLAGS " --sysroot=${MCPP_SYSROOT}")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " --sysroot=${MCPP_SYSROOT}")
endif()

# ---------------------------------------------------------------------------
# Source set — mcpp.toml's inferred glob `src/**/*.{cppm,cpp}`. mcpp infers
# kind=bin from src/main.cpp; CMake needs it spelled out.
#
# GLOB, not a hand-written list: the two arms must compile the same files even
# as the tree changes, and a list that drifts silently measures two different
# projects. CONFIGURE_DEPENDS re-globs on build so an added module is not missed.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MCPP_MODULES CONFIGURE_DEPENDS "${MCPP_ROOT}/src/*.cppm")
list(LENGTH MCPP_MODULES MCPP_MODULE_COUNT)
if(MCPP_MODULE_COUNT EQUAL 0)
  message(FATAL_ERROR "no module interface units found under src/ — refusing to "
                      "build a project that is not mcpp")
endif()

# mcpp.toml pins `mcpplibs.cmdline = "0.0.1"` EXACTLY. Newer versions are often
# also unpacked in the registry, so pin rather than take the newest: otherwise
# the two arms are not compiling the same code.
#
# mcpp stages prebuilt objects for this dependency out of its global build cache
# and cmake has no such cache, so cmake compiles the 3 units from source. That is
# a small handicap on cmake's cold build, and it is declared in the benchmark
# report rather than hidden.
set(MCPP_CMDLINE_VERSION "0.0.1")
set(MCPP_CMDLINE_SRC
    "${MCPP_XPKGS}/mcpplibs-x-cmdline/${MCPP_CMDLINE_VERSION}/cmdline-${MCPP_CMDLINE_VERSION}/src")
if(IS_DIRECTORY "${MCPP_CMDLINE_SRC}")
  file(GLOB MCPP_CMDLINE_MODULES CONFIGURE_DEPENDS "${MCPP_CMDLINE_SRC}/*.cppm")
else()
  message(WARNING "mcpplibs.cmdline ${MCPP_CMDLINE_VERSION} not unpacked at "
                  "${MCPP_CMDLINE_SRC}; this build will not match mcpp's own")
  set(MCPP_CMDLINE_MODULES "")
endif()

add_executable(mcpp "${MCPP_ROOT}/src/main.cpp")

# FILE_SET CXX_MODULES is the only way CMake learns these are interface units.
# Listing them as ordinary sources compiles them as plain TUs and the link fails
# with missing module symbols.
target_sources(mcpp
  PRIVATE
    FILE_SET CXX_MODULES BASE_DIRS "${MCPP_ROOT}/src" FILES ${MCPP_MODULES}
)

# The dependency's units need their OWN file set: a CXX_MODULES set requires
# every file to live under one of its base directories, which defaults to the
# project source dir, and these live in the registry outside the tree.
if(MCPP_CMDLINE_MODULES)
  target_sources(mcpp
    PRIVATE
      FILE_SET mcpp_cmdline_modules
        TYPE CXX_MODULES
        BASE_DIRS "${MCPP_CMDLINE_SRC}"
        FILES ${MCPP_CMDLINE_MODULES}
  )
endif()

# mcpp.toml: include_dirs = ["src/libs/json"] — src/libs/json.cppm reaches for
# <json.hpp> from its global module fragment.
target_include_directories(mcpp PRIVATE "${MCPP_ROOT}/src/libs/json")

# mcpp.toml default: static_stdlib = true, so the binary is portable.
target_link_options(mcpp PRIVATE -static-libstdc++)

message(STATUS "mcpp: ${MCPP_MODULE_COUNT} module interface units + src/main.cpp")
