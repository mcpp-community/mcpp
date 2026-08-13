-- xmake build description for xlings — the benchmark's independent control target.
--
-- Counterpart to CMakeLists.txt here, and to bench/projects/mcpp/xmake.lua. The
-- fairness contract is the same five: same compiler binary, same language
-- flags, same source set, same link output kind, same standard library
-- (`import std;`, not a header shim).
--
-- THE TREE IS NOT VENDORED, so unlike bench/projects/mcpp/xmake.lua this cannot
-- derive its root from os.scriptdir(). Point it at a checkout:
--
--   XLINGS_ROOT=/path/to/xlings xmake f -P bench/projects/xlings -y -m release
--   XLINGS_ROOT=/path/to/xlings xmake build -P bench/projects/xlings -j32
--
-- Record the commit with the numbers; the published ones are from `b1563fe`.
--
-- ⚠️ SAME STATUS AS THE CMAKE ARM: every translation unit compiles; the LINK
-- does not. ftxui / libarchive / lua / mbedtls arrive as SOURCE and mcpp
-- compiles them, so the link wants symbols nobody built here. That is ordinary
-- work, not a wall — and it is the same gap in both foreign arms, which is why
-- the xlings numbers are quoted as mcpp-vs-mcpp (see README.md).

set_project("xlings")
set_xmakever("2.9.0")
set_languages("c++23")
add_rules("mode.debug", "mode.release")

-- The hermetic payload and the toolchain-per-family definitions are SHARED with
-- the mcpp arm: ../common/xmake/payload.lua.
includes("../common/xmake/payload.lua")

-- BENCH_PROJECT_ROOT is what the harness exports for every --project run, and
-- it is why one description serves both pinned trees. XLINGS_ROOT stays
-- supported for driving this by hand.
local XLINGS_ROOT = os.getenv("BENCH_PROJECT_ROOT") or os.getenv("XLINGS_ROOT")
local XLINGS_MANIFEST = XLINGS_ROOT and path.join(XLINGS_ROOT, "mcpp.toml")

option("pin_payload")
    set_default(true)
    set_showmenu(true)
    set_description("Pin the hermetic mcpp toolchain payload (required for a fair benchmark)")
option_end()

bench_define_toolchains(XLINGS_MANIFEST)

-- ── Everything the helpers answer is resolved HERE, at description scope ──────
--
-- ⚠️ THE HELPERS ARE NOT REACHABLE FROM on_load/before_build. xmake runs those
-- callbacks in a sandbox that does not carry an include()'d file's globals, so
-- `bench_package_root(...)` inside one fails with
--
--     error: attempt to call a nil value (global 'bench_package_root')
--
-- and the whole xlings/xmake arm reported `configure exited 255` — in CI, for
-- every cell, behind a green check. Lua closures capture their upvalues
-- lexically, so resolving to LOCALS here and letting the target read those is
-- both the fix and the shape bench/projects/mcpp/xmake.lua already used.
-- ── Dependencies, declared the way xlings itself declares them ───────────────
--
-- Shaped after openxlings/xlings@bb27e43's own xmake.lua: `add_requires` for
-- every dependency, `add_packages` on the target. Compiling them out of mcpp's
-- registry by hand — the previous shape here — went wrong in five separate ways
-- (source lists that a glob gets wrong, `!` exclusions that `target:add` ignores,
-- escaped quotes in cflags, `*`-prefixed paths resolving to the filesystem root,
-- and `io` being nil in description scope) before producing a binary. This is
-- what an xmake user would actually write.
--
-- The index needed three newer versions than it carried (mcpplibs-xpkg 0.0.57,
-- capi-lua 0.0.3, tinyhttps 0.2.9); they were added there rather than overridden
-- here, because every xmake user of these libraries needs them, not just this
-- benchmark. mcpplibs-index also had to learn to SUPPLY a build description for
-- mcpplibs-xpkg: libxpkg moved to mcpp, so its 0.0.57 tarball ships `mcpp.toml`
-- and no xmake.lua at all.
--
-- (mcpplibs/mcpplibs-index#14, merged.) MCPPLIBS_INDEX still overrides the URL,
-- which is how the next version bump gets tested against a checkout before it
-- is published.
includes("packages/libarchive.lua")

-- EVERY dependency static, transitively. Without this the binary builds and
-- then cannot start:
--     error while loading shared libraries: libbz2.so.1.0: cannot open
--     shared object file: No such file or directory
-- bzip2 arrives through libarchive and xrepo built it shared, so the link
-- succeeded against a .so that is not on any runtime path. It also matters for
-- the comparison: mcpp produces a self-contained binary here (the arm passes
-- `-static-libstdc++` below), so an arm that leaves its dependencies dynamic is
-- not producing the same artifact.
add_requireconfs("**", {configs = {shared = false}})
add_repositories("mcpplibs-index " ..
                 (os.getenv("MCPPLIBS_INDEX") or "https://github.com/mcpplibs/mcpplibs-index.git"))

add_requires("cmdline 0.0.2")
add_requires("mcpplibs-capi-lua 0.0.3")
add_requires("mcpplibs-tinyhttps 0.2.9")
add_requires("mcpplibs-xpkg 0.0.57")
add_requires("ftxui 6.1.9")
-- libarchive-xlings, not plain libarchive: xmake-repo's build stops at
-- `CMake Error at CMakeLists.txt:1349 (MESSAGE): libgcc not found.` under the
-- payload toolchain. The override is xlings' own (see packages/libarchive.lua).
add_requires("libarchive-xlings 3.8.7")


target("xlings")
    set_kind("binary")

    if not XLINGS_ROOT or not os.isfile(XLINGS_MANIFEST) then
        raise("no xlings tree: set XLINGS_ROOT=<dir>, or let the bench harness "
              .. "export BENCH_PROJECT_ROOT via --project. The pinned trees are "
              .. "the submodules bench/projects/xlings/xlings-<version>/ — run "
              .. "`git submodule update --init`.")
    end

    -- Source set == xlings' mcpp.toml inferred glob src/**/*.{cppm,cpp}; mcpp
    -- infers kind=bin from src/main.cpp, xmake needs it spelled out.
    --
    -- BOTH extensions, which is what lets ONE description measure xlings' two
    -- code styles: 2026.8.11.2 has 110 .cppm + 2 .cpp (implementation inside
    -- each interface unit), 2026.8.13.1 has 110 .cppm + 92 .cpp (split out).
    -- Globbing `src/main.cpp` alone would compile the interfaces of the split
    -- style, link nothing, and still report a number. Same note in CMakeLists.txt.
    add_files(path.join(XLINGS_ROOT, "src/**.cppm"))
    add_files(path.join(XLINGS_ROOT, "src/**.cpp"))

    -- Every dependency comes through xrepo, exactly as xlings' own xmake.lua
    -- does. `mcpplibs-xpkg` brings the generated `mcpplibs.xpkg.lua_stdlib`
    -- module with it — that generation lives in the PACKAGE now
    -- (xrepo/packages/m/mcpplibs-xpkg/xmake.lua), which is where libxpkg keeps
    -- it too, instead of being re-implemented against the registry here.
    add_packages("cmdline", "mcpplibs-capi-lua", "mcpplibs-tinyhttps",
                 "mcpplibs-xpkg", "ftxui", "libarchive-xlings")

    -- `[build] include_dirs = ["src/libs/json"]` — src/libs/json.cppm reaches
    -- for <json.hpp> from its global module fragment.
    add_includedirs(path.join(XLINGS_ROOT, "src/libs/json"))
    -- `[build] cxxflags`
    add_defines("LIBARCHIVE_STATIC", "UNICODE", "_UNICODE")


    -- `mcpplibs.xpkg.lua_stdlib` is GENERATED by libxpkg's build.mcpp rather
    -- than checked in: it embeds every .lua under src/lua-stdlib as a string
    -- named after the file. Same RULE as embed_lua_stdlib.cmake (which the cmake
    -- arm runs) — deliberately not the same list, because a copied list here
    -- already drifted once and the failure landed three files away, in a
    -- consumer, as `'base64_lua' is not a member of ...detail`.
    --
    -- on_load, NOT before_build. The file existing early is only half of what is

    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", true)

    add_ldflags("-static-libstdc++", {force = true})

    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("hidden")
    elseif is_mode("debug") then
        set_optimize("none")
        set_symbols("debug")
    end

    -- Which toolchain, and the rule for when NOT to pin one, live in
    -- ../common/xmake/payload.lua.
    if has_config("pin_payload") then
        local tc = bench_pinned_toolchain()
        if tc then set_toolchains(tc) end
    end
target_end()
