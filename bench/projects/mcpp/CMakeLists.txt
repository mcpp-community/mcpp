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

# The DIALECT settings belong before project() for the same reason as the key
# above, and getting that wrong is not a style question — it is why this arm
# could not build at all.
#
# CMake synthesises its own target for the std module during the compiler probe
# inside project(). That target captures whatever CMAKE_CXX_EXTENSIONS says AT
# THAT MOMENT, and the default is ON. Set OFF afterwards, std.pcm is built as
# `gnu++23` while every real target compiles as `c++23`, and clang refuses the
# mismatch:
#
#   error: GNU extensions was enabled in precompiled file 'std.pcm'
#          but is currently disabled
#   error: precompiled file 'std.pcm' cannot be loaded due to a configuration
#          mismatch with the current compilation
#
# `import std;` then supplies nothing and the build dies in 19 places with
# `use of undeclared identifier 'std'` — an error that points at mcpp's sources
# and names neither std.pcm nor extensions.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(mcpp CXX)
# Every mcpp module says `import std;`. This asks CMake to build the standard
# library module from the compiler's own libstdc++.modules.json, which the
# hermetic gcc payload ships.
set(CMAKE_CXX_MODULE_STD 1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Where the tree is, and the hermetic payload that makes this a fair comparison.
#
# The payload logic is SHARED with the xlings arm — see
# ../common/cmake/hermetic_payload.cmake, including why it is one branch per
# compiler family rather than one block of flags, and why it must be applied
# through CMAKE_CXX_FLAGS.
# ---------------------------------------------------------------------------
# The tree is whatever the harness points at — BENCH_PROJECT_ROOT, exported for
# every `--project` run. It used to be derived from this file's own location
# (`../../..`, i.e. the checkout), and that made mcpp's WORKING TREE the
# workload: every commit on a branch silently changed the thing being measured,
# so no two runs on that branch were comparable to each other.
#
# A benchmark has two halves. The engine under test is the binary and is
# supposed to move; the workload is not. It is now the pinned submodule
# `mcpp-<version>/` beside this file, exactly like the xlings arms.
#
# Resolved to an absolute path, because a FILE_SET's base directory and a
# relative glob disagree about what "here" means.
if(NOT MCPP_ROOT AND DEFINED ENV{BENCH_PROJECT_ROOT})
  set(MCPP_ROOT "$ENV{BENCH_PROJECT_ROOT}")
endif()
if(NOT MCPP_ROOT)
  # Driving this by hand: default to the pinned workload rather than to the
  # checkout, so the hand-run and the CI run measure the same sources.
  file(GLOB pinned "${CMAKE_CURRENT_SOURCE_DIR}/mcpp-*")
  if(pinned)
    list(SORT pinned)
    list(GET pinned -1 MCPP_ROOT)
  endif()
endif()
get_filename_component(MCPP_ROOT "${MCPP_ROOT}" ABSOLUTE)
if(NOT MCPP_ROOT OR NOT EXISTS "${MCPP_ROOT}/mcpp.toml")
  message(FATAL_ERROR
    "no mcpp tree at '${MCPP_ROOT}': set -DMCPP_ROOT=<dir>, or let the bench "
    "harness export BENCH_PROJECT_ROOT via --project. The pinned workload is the "
    "submodule bench/projects/mcpp/mcpp-<version>/ — run `git submodule update --init`.")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../common/cmake/hermetic_payload.cmake)
bench_hermetic_payload()
bench_registry_xpkgs(MCPP_XPKGS)

# ---------------------------------------------------------------------------
# Source set — mcpp.toml's inferred glob `src/**/*.{cppm,cpp}`. mcpp infers
# kind=bin from src/main.cpp; CMake needs it spelled out.
#
# GLOB, not a hand-written list: the two arms must compile the same files even
# as the tree changes, and a list that drifts silently measures two different
# projects. CONFIGURE_DEPENDS re-globs on build so an added module is not missed.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MCPP_MODULES CONFIGURE_DEPENDS "${MCPP_ROOT}/src/*.cppm")
# .cpp is globbed for the same reason, even though mcpp has exactly one today:
# the xlings arm found this the hard way. Naming `src/main.cpp` by hand keeps
# working right up until implementations are split out of the interface units,
# and then it compiles the interfaces, links nothing, and still reports a time.
file(GLOB_RECURSE MCPP_SOURCES CONFIGURE_DEPENDS "${MCPP_ROOT}/src/*.cpp")
list(LENGTH MCPP_MODULES MCPP_MODULE_COUNT)
list(LENGTH MCPP_SOURCES MCPP_SOURCE_COUNT)
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

add_executable(mcpp ${MCPP_SOURCES})

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

message(STATUS "mcpp: ${MCPP_MODULE_COUNT} module interface units + "
               "${MCPP_SOURCE_COUNT} .cpp")
