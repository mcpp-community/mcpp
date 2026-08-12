# meson description for mcpp — BEST EFFORT, AND IT DOES NOT BUILD.
#
# Kept so the directory answers "what about meson?" with a measurement instead
# of silence, and so the day meson grows the feature this file is the diff.
#
# Two independent blockers, both measured on meson 1.10.2:
#
#   1. No named modules. meson has no attribute that declares a translation unit
#      to be a module INTERFACE. Listing .cppm files as ordinary sources compiles
#      them as plain TUs, and the first importer fails with
#          fatal error: module 'mcpp.log' not found
#      This is the same failure the synthetic fixture hits; see
#      bench/README.md §5.
#
#   2. No `import std;`. Every one of mcpp's 138 interface units imports the
#      standard library module. CMake needs an experimental UUID plus the
#      compiler's libstdc++.modules.json for this; meson has no equivalent.
#
# Blocker 1 alone is fatal, so the harness reports meson as `unavailable` with
# the reason rather than running this and reporting a failed build.

project('mcpp', 'cpp',
  version: '2026.8.12.1',
  default_options: ['cpp_std=c++23', 'buildtype=release'])

fs = import('fs')
root = meson.current_source_dir() / '..' / '..' / '..'

# Enumerated rather than globbed: meson deliberately has no glob, and hard-coding
# 138 paths in a file that cannot build anyway would be noise. run_command with
# `find` would work and is left out for the same reason.
error('meson 1.10.2 cannot build C++20 named modules (no interface-unit '
    + 'declaration) and has no `import std;` support; see the comment above. '
    + 'This file exists to record that, not to build mcpp.')
