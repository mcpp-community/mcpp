# Shared by every CMake arm in bench/projects/.
#
# WHY THIS IS SHARED. mcpp's arm and xlings' arm need exactly the same thing —
# "make cmake drive the same process tree mcpp does" — and they had two copies
# of it. The copies were already diverging (one had learned about the `std`
# module / CMAKE_CXX_FLAGS trap, the other had not), and a benchmark whose two
# arms are configured differently is measuring the difference between the two
# descriptions.
#
# WHAT IT IS FOR. mcpp resolves a compiler out of its own registry and always
# passes the rest of the payload explicitly. A bare compiler from that registry
# falls back to PATH for `as`/`ld` and to the host for headers, so the two arms
# would compile the same sources against different libc. Reproducing the payload
# here is what makes the wall-clock difference attributable to the build engine.
#
# THE COMPILER IS AN AXIS, so this is not one block of flags — it is one per
# family. Getting it wrong is not a build failure, it is a slower or faster
# number with no visible cause.
#
# Usage, BEFORE the first target:
#     include(${CMAKE_CURRENT_LIST_DIR}/../common/cmake/hermetic_payload.cmake)
#     bench_hermetic_payload()

# Where mcpp keeps its packages. `MCPP_HOME` first, matching mcpp's own
# resolution order.
function(bench_registry_xpkgs out)
  if(DEFINED ENV{MCPP_HOME})
    set(home "$ENV{MCPP_HOME}")
  elseif(WIN32)
    set(home "$ENV{USERPROFILE}/.mcpp")
  else()
    set(home "$ENV{HOME}/.mcpp")
  endif()
  set(${out} "${home}/registry/data/xpkgs" PARENT_SCOPE)
endfunction()

# The newest unpacked version of a package, or "" — used only for payload
# components (binutils, glibc headers), never for a dependency whose version the
# manifest pins. "Newest directory wins" is fine for a toolchain payload and is
# NOT fine for a dependency: the registry holds several versions and picking the
# lexically-last one only happens to agree with the pin.
function(bench_newest_package xpkgs name out)
  file(GLOB dirs "${xpkgs}/${name}/*")
  set(${out} "" PARENT_SCOPE)
  if(dirs)
    list(SORT dirs)
    list(GET dirs -1 newest)
    set(${out} "${newest}" PARENT_SCOPE)
  endif()
endfunction()

function(bench_hermetic_payload)
  bench_registry_xpkgs(xpkgs)
  set(sysroot "")
  if(DEFINED ENV{MCPP_HOME})
    set(sysroot "$ENV{MCPP_HOME}/registry/subos/default")
  elseif(NOT WIN32)
    set(sysroot "$ENV{HOME}/.mcpp/registry/subos/default")
  endif()

  # A compiler from OUTSIDE the registry is the caller's explicit opt-in to the
  # host world — the same rule mcpp's own hermetic link check applies. Adding a
  # registry sysroot to a host g++ produces a mixed build that fails somewhere
  # unrelated, so say nothing instead.
  get_filename_component(cxx_real "${CMAKE_CXX_COMPILER}" REALPATH)
  string(FIND "${cxx_real}" "xpkgs" xpkgs_pos)
  if(xpkgs_pos EQUAL -1)
    message(STATUS "bench: ${CMAKE_CXX_COMPILER_ID} compiler is outside mcpp's "
                   "registry — using it as-is (no payload flags)")
    return()
  endif()

  # ── MSVC ────────────────────────────────────────────────────────────────
  # There is no payload: mcpp uses the SYSTEM Visual Studio installation
  # (`msvc@system`), reached through the VS environment rather than through
  # flags. Both arms therefore already share it, and there is nothing to add.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    return()
  endif()

  # CMAKE_CXX_FLAGS, not add_compile_options(): CMake generates the `std` module
  # target ITSELF, and directory-scope options do not reach it. Without this the
  # std module compiles against whatever libc headers the compiler defaults to
  # while every project unit compiles against the payload, and the build dies on
  # a type that exists in both:
  #
  #   error: conflicting type for imported declaration 'char _IO_FILE::_unused2 [20]'
  #     .../glibc-2.39/include/bits/types/struct_FILE.h:98
  #     note: existing declaration 'char _IO_FILE::_unused2 [8]'
  #     .../registry/subos/default/usr/include/bits/types/struct_FILE.h:109
  #
  # Two glibcs in one link, and the error names neither the flag nor the target
  # that is wrong.
  set(cxx "${CMAKE_CXX_FLAGS}")
  set(ld  "${CMAKE_EXE_LINKER_FLAGS}")

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # -B and --sysroot must reach BOTH compile and link: the driver spawns `as`
    # from it at compile time and `ld` from it at link time. Adding it on one
    # side only silently falls through to PATH.
    bench_newest_package("${xpkgs}" "xim-x-binutils" binutils)
    if(binutils)
      string(APPEND cxx " -B${binutils}/bin")
      string(APPEND ld  " -B${binutils}/bin")
    endif()
    if(IS_DIRECTORY "${sysroot}")
      string(APPEND cxx " --sysroot=${sysroot}")
      string(APPEND ld  " --sysroot=${sysroot}")
    endif()

  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang's payload is shaped differently and `--sysroot` is NOT the
    # equivalent: mcpp drives clang with an explicit include chain instead
    # (verified against a real mcpp build command). Passing gcc's --sysroot to
    # clang here would be the mirror of the bug above — one arm on the payload
    # libc, the other on the host's.
    #
    # The llvm root is derived from the COMPILER PATH, not globbed: the registry
    # can hold 20.1.7 and 22.1.8 at once, and "newest wins" would silently
    # compile against a different libc++ than the driver being measured.
    get_filename_component(llvm_bin "${cxx_real}" DIRECTORY)
    get_filename_component(llvm_root "${llvm_bin}" DIRECTORY)
    if(IS_DIRECTORY "${llvm_root}/include/c++/v1")
      string(APPEND cxx " --no-default-config -nostdinc++")
      string(APPEND cxx " -isystem${llvm_root}/include/c++/v1")
      # The per-triple directory carries __config_site; it is absent on some
      # builds, so it is added only when present rather than unconditionally.
      file(GLOB triple_inc "${llvm_root}/include/*/c++/v1")
      foreach(d IN LISTS triple_inc)
        string(APPEND cxx " -isystem${d}")
      endforeach()
      string(APPEND ld " -nostdlib++ -L${llvm_root}/lib -lc++ -lc++abi")
      # The unwinder ships beside libc++ in this payload; without it the link
      # fails on _Unwind_Resume, which reads as a missing exception runtime
      # rather than as a missing -L.
      if(EXISTS "${llvm_root}/lib/libunwind.so" OR EXISTS "${llvm_root}/lib/libunwind.a")
        string(APPEND ld " -lunwind")
      endif()
    endif()
    bench_newest_package("${xpkgs}" "xim-x-glibc" glibc)
    if(glibc AND IS_DIRECTORY "${glibc}/include")
      string(APPEND cxx " -isystem${glibc}/include")
    endif()
    bench_newest_package("${xpkgs}" "xim-x-linux-headers" uapi)
    if(uapi AND IS_DIRECTORY "${uapi}/include")
      string(APPEND cxx " -isystem${uapi}/include")
    endif()
  endif()

  set(CMAKE_CXX_FLAGS        "${cxx}" PARENT_SCOPE)
  set(CMAKE_EXE_LINKER_FLAGS "${ld}"  PARENT_SCOPE)
  message(STATUS "bench: hermetic payload for ${CMAKE_CXX_COMPILER_ID} applied")
endfunction()

# One source dependency out of the registry, added to `target` as its own
# CXX_MODULES file set.
#
# Its own set, because a CXX_MODULES set requires every file to live under one
# of its base directories and these sit in the registry, outside the tree.
#
# The VERSION IS PINNED BY THE CALLER, from the manifest — see the warning in
# bench_newest_package about why "newest wins" is wrong here.
function(bench_add_source_dep target name version)
  bench_registry_xpkgs(xpkgs)
  set(dir "${xpkgs}/${name}/${version}")
  file(GLOB_RECURSE srcs CONFIGURE_DEPENDS "${dir}/*/src/*.cppm")
  if(NOT srcs)
    message(WARNING "dependency ${name} ${version} is not unpacked at ${dir}; "
                    "this build will not match mcpp's own")
    return()
  endif()
  list(GET srcs 0 first)
  get_filename_component(base "${first}" DIRECTORY)
  # A file-set name may only contain letters, digits and underscores — package
  # names like `mcpplibs.capi-x-lua` do not qualify, and CMake rejects them at
  # configure time rather than mangling them.
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" fsname "fs_${name}")
  target_sources(${target} PRIVATE
    FILE_SET "${fsname}" TYPE CXX_MODULES BASE_DIRS "${base}" FILES ${srcs})
endfunction()
