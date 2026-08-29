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

-- The tree this file builds is whatever the harness points at —
-- BENCH_PROJECT_ROOT, exported for every `--project` run.
--
-- It used to be `os.scriptdir()/../../..`, i.e. the checkout, which made mcpp's
-- WORKING TREE the workload: every commit on a branch silently changed the thing
-- being measured, so no two runs on that branch were comparable. The engine
-- under test is the binary and is supposed to move; the workload is not.
--
-- Driving this by hand falls back to the pinned submodule beside this file
-- rather than to the checkout, so a hand-run and a CI run measure the same
-- sources.
local MCPP_ROOT = os.getenv("BENCH_PROJECT_ROOT") or os.getenv("MCPP_ROOT")
if not MCPP_ROOT then
    local pinned = os.dirs(path.join(os.scriptdir(), "mcpp-*"))
    if pinned and #pinned > 0 then
        table.sort(pinned)
        MCPP_ROOT = pinned[#pinned]
    end
end
if not MCPP_ROOT or not os.isfile(path.join(MCPP_ROOT, "mcpp.toml")) then
    raise("no mcpp tree: set MCPP_ROOT=<dir>, or let the bench harness export "
          .. "BENCH_PROJECT_ROOT via --project. The pinned workload is the "
          .. "submodule bench/projects/mcpp/mcpp-<version>/ — run "
          .. "`git submodule update --init`.")
end
MCPP_ROOT = path.normalize(MCPP_ROOT)
local MCPP_MANIFEST = path.join(MCPP_ROOT, "mcpp.toml")

-- Which version of mcpplibs.cmdline to compile is read from the measured tree's
-- own mcpp.lock, NOT hardcoded and NOT "the newest unpacked".
--
--   * hardcoding it is what broke CI: this said "0.0.1" because mcpp.toml says
--     `mcpplibs.cmdline = "0.0.1"`, but that is a REQUIREMENT, not a resolution.
--     A developer box that had 0.0.1 unpacked from some earlier run worked; a
--     fresh runner had only what mcpp resolved, `bench_package_root` returned
--     nil, the `if` below quietly added no files, and the build died 137 units
--     later with `missing mcpplibs.cmdline dependency for module mcpp.cli` —
--     an error that names neither the version nor the registry.
--   * "the newest unpacked" would silently compile different sources than mcpp
--     did, which is the one thing a comparison arm may not do.
--
-- The lockfile is the resolution mcpp itself performed on this exact tree, so
-- both arms compile the same code by construction.
-- Read inside on_load, not here: `io` is nil in xmake's DESCRIPTION scope, so a
-- reader written at this level dies with `attempt to index a nil value (global
-- 'io')` — the same trap ../common/xmake/payload.lua documents for its manifest
-- reader, walked into a second time. Both the lock path and the registry root
-- are captured as upvalues for the same reason: on_load's sandbox cannot see
-- this file's globals, so `bench_package_root` is not callable from in there.
local MCPP_LOCK = path.join(MCPP_ROOT, "mcpp.lock")
local XPKGS     = bench_xpkgs()

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
    -- `modules/` holds packages mcpp links into itself, each with its own
    -- manifest. The comparison has to compile the same set of translation
    -- units mcpp does, so they are added here as plain sources: xmake has no
    -- package boundary to model, and a missing boundary is not a missing unit.
    add_files(path.join(MCPP_ROOT, "modules/*/src/**.cppm"))

    -- modules/json/mcpp.toml: include_dirs = ["src/json"] — its module reaches
    -- for <json.hpp> from its global module fragment. Private to that package
    -- in mcpp's build; a flat include path here.
    add_includedirs(path.join(MCPP_ROOT, "modules/json/src/json"))

    -- mcpp.toml: [dependencies] mcpplibs.cmdline = "0.0.1".
    -- mcpp stages prebuilt objects for this out of its global build cache; xmake
    -- has no such cache, so it compiles the 3 units from source. That is a ~1s
    -- handicap on xmake's cold build and is called out in the benchmark report
    -- rather than hidden.
    --
    -- Absence is FATAL rather than skipped. Skipping produced a build missing
    -- three units out of 140 that announced itself only as
    -- `missing mcpplibs.cmdline dependency for module mcpp.cli.cmd_cache` —
    -- naming a consumer instead of the cause. A description that cannot name the
    -- same sources mcpp compiled is not a comparison arm.
    on_load(function (target)
        local ver
        if os.isfile(MCPP_LOCK) then
            local in_section = false
            for _, line in ipairs((io.readfile(MCPP_LOCK) or ""):split("\n", {plain = true})) do
                local section = line:match("^%s*%[(.-)%]")
                if section then in_section = (section == 'package."mcpplibs.cmdline"')
                elseif in_section then
                    ver = ver or line:match('^%s*version%s*=%s*"([^"]+)"')
                end
            end
        end
        if not ver then
            raise("bench: cannot read mcpplibs.cmdline's resolved version from " .. MCPP_LOCK
                  .. " — the measured tree must carry the lockfile mcpp resolved it with")
        end
        local base = path.join(XPKGS, "mcpplibs-x-cmdline", ver)
        local dirs = os.isdir(base) and os.dirs(path.join(base, "*")) or {}
        table.sort(dirs)

        -- NAME THE DIRECTORY, DO NOT SEARCH FOR IT.
        --
        -- `cmdline-<version>` is the registry's layout and is exactly what the
        -- cmake arm beside this one writes down — which is why cmake's five
        -- cells were green in the same run where these five were red. Two arms
        -- that must compile the SAME sources cannot locate them two ways.
        --
        -- Searching was wrong twice over. `dirs[1]` after a sort is "whatever
        -- happens to come first", and the registry keeps a tarball and a lock
        -- beside the unpacked tree while mcpp writes partial directories during
        -- unpacking. Widening it to "the first directory that has a src/" is no
        -- better: renaming the real tree to `cmdline-0.0.1.hidden` to test the
        -- error path made this file compile THAT instead, silently, and report
        -- success. A backup directory is a plausible thing to find on a machine.
        local canonical = path.join(base, "cmdline-" .. ver, "src")
        local src = os.isdir(canonical) and canonical or nil
        if not src then
            -- ⚠️ SAY WHAT WAS LOOKED AT. The previous message named the version
            -- and the registry root and stopped there, so a CI failure could not
            -- be told apart from "the package is genuinely absent", "the version
            -- came out wrong", or "the directory is there but holds something
            -- else". Three different causes, one sentence, none of them
            -- actionable without a runner to log into.
            local found = #dirs > 0 and table.concat(dirs, ", ") or "(nothing)"
            raise("bench: mcpplibs.cmdline " .. ver .. " has no unpacked source tree.\n"
                  .. "  expected: " .. canonical .. "\n"
                  .. "  directories under " .. base .. ": " .. found .. "\n"
                  .. "  base exists: " .. tostring(os.isdir(base)) .. "\n"
                  .. "  Build the tree with mcpp once first, so both arms compile the "
                  .. "same dependency sources. A cache hit does NOT unpack them.")
        end
        target:add("files", path.join(src, "*.cppm"))
    end)

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
