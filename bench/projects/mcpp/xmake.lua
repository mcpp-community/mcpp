-- xmake build description for mcpp — a like-for-like counterpart to mcpp.toml.
--
-- Lives under bench/projects/mcpp/ rather than at the repository root: mcpp is
-- built by mcpp, and a second build description at the root is something a
-- contributor has to learn to ignore. It is used only by the benchmark.
--
-- Why this file exists: it is the control arm of the build-engine benchmark in
-- tools/bench/. mcpp builds itself; this makes xmake build the exact same 137
-- module interface units + src/main.cpp with the exact same compiler binary, so
-- any wall-clock difference is attributable to the build engine (graph shape,
-- scheduling, staleness model) and not to a different toolchain.
--
-- Fairness contract (all four must hold or the comparison is meaningless):
--   1. same compiler binary   -- pinned below to the hermetic payload mcpp resolves
--   2. same language flags    -- -std=c++23 -fmodules -O2 (release) / -O0 -g (debug)
--   3. same source set        -- src/**.cppm + src/main.cpp + the cmdline dependency
--   4. same link output kind  -- one binary, -static-libstdc++
--
-- Usage (benchmark), from the repository root:
--   xmake f -P bench/projects/mcpp -y -m release --toolchain=mcpp-gcc
--   xmake build -P bench/projects/mcpp -j32
-- Usage (plain host toolchain, no pinning):
--   xmake f -y -m release --pin_payload=n && xmake build

set_project("mcpp")
set_xmakever("2.9.0")
set_languages("c++23")
add_rules("mode.debug", "mode.release")

-- ---------------------------------------------------------------------------
-- The hermetic payload and the toolchain-per-family definitions are SHARED with
-- the xlings arm: ../common/xmake/payload.lua. They used to be a 60-line copy in
-- each file, which is the worst place for a copy — two toolchain definitions
-- drift by one flag and the benchmark reports the difference between the two
-- DESCRIPTIONS as an engine result.
-- ---------------------------------------------------------------------------
includes("../common/xmake/payload.lua")

-- The tree this file builds. os.scriptdir() is bench/projects/mcpp, so the
-- repository root is three levels up; deriving it from the SCRIPT rather than
-- from the working directory keeps `xmake -P` working from anywhere.
local MCPP_ROOT     = path.normalize(path.join(os.scriptdir(), "..", "..", ".."))
local MCPP_MANIFEST = path.join(MCPP_ROOT, "mcpp.toml")

-- mcpp.toml pins mcpplibs.cmdline = "0.0.1" exactly; newer versions may also be
-- unpacked in the registry, so pin rather than take the newest or the two builds
-- would not be compiling the same code.
local CMDLINE_VER = "0.0.1"
local CMDLINE_SRC = bench_package_root("mcpplibs-x-cmdline", CMDLINE_VER)

option("pin_payload")
    set_default(true)
    set_showmenu(true)
    set_description("Pin the hermetic mcpp toolchain payload (required for a fair benchmark)")
option_end()

bench_define_toolchains(MCPP_MANIFEST)

-- ---------------------------------------------------------------------------
-- The one and only target: mcpp's CLI binary.
-- ---------------------------------------------------------------------------
target("mcpp")
    set_kind("binary")

    -- Source set == mcpp.toml's inferred glob src/**/*.{cppm,cpp}. mcpp infers
    -- kind=bin from src/main.cpp; xmake needs it spelled out.
    add_files(path.join(MCPP_ROOT, "src/**.cppm"))
    -- .cpp is globbed even though mcpp has exactly one today: naming it by
    -- hand keeps working right until implementations are split out of the
    -- interface units (which is what xlings did), and then this compiles
    -- the interfaces, links nothing, and still reports a time.
    add_files(path.join(MCPP_ROOT, "src/**.cpp"))

    -- mcpp.toml: include_dirs = ["src/libs/json"] — src/libs/json.cppm reaches
    -- for <json.hpp> from its global module fragment.
    add_includedirs(path.join(MCPP_ROOT, "src/libs/json"))

    -- mcpp.toml: [dependencies] mcpplibs.cmdline = "0.0.1".
    -- mcpp stages prebuilt objects for this out of its global build cache; xmake
    -- has no such cache, so it compiles the 3 units from source. That is a ~1s
    -- handicap on xmake's cold build and is called out in the benchmark report
    -- rather than hidden.
    if CMDLINE_SRC and os.isdir(path.join(CMDLINE_SRC, "src")) then
        add_files(path.join(CMDLINE_SRC, "src", "*.cppm"))
    end

    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", true)

    -- mcpp.toml default: static_stdlib = true (portable binary).
    add_ldflags("-static-libstdc++", {force = true})

    if is_mode("release") then
        set_optimize("fastest")          -- -O2, matching mcpp's release profile
        set_symbols("hidden")
    elseif is_mode("debug") then
        set_optimize("none")             -- -O0 -g, matching mcpp's dev profile
        set_symbols("debug")
    end

    -- Which toolchain, and the rule for when NOT to pin one, live in
    -- ../common/xmake/payload.lua — an unconditional set_toolchains() here
    -- silently overrides `xmake f --toolchain=...` and the benchmark reports a
    -- cell compiled by the wrong compiler.
    if has_config("pin_payload") then
        local tc = bench_pinned_toolchain()
        if tc then set_toolchains(tc) end
    end
target_end()
