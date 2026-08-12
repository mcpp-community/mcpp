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
-- Where mcpp keeps its hermetic toolchain payload. mcpp resolves gcc@16.1.0 to
-- $MCPP_HOME/registry/data/xpkgs/xim-x-gcc/<ver>/bin/g++ and always passes an
-- explicit -B<binutils> plus --sysroot; a bare `g++` from that payload falls back
-- to PATH for `as`/`ld` and picks up whatever shim is there. We reproduce the
-- full triple (compiler + binutils + sysroot) so xmake drives an identical
-- process tree.
-- ---------------------------------------------------------------------------
-- The tree this file builds. os.scriptdir() is bench/projects/mcpp, so the
-- repository root is three levels up; deriving it from the SCRIPT rather than
-- from the working directory keeps `xmake -P` working from anywhere.
local MCPP_ROOT   = path.normalize(path.join(os.scriptdir(), "..", "..", ".."))
local MCPP_HOME   = os.getenv("MCPP_HOME") or path.join(os.getenv("HOME"), ".mcpp")
local XPKGS       = path.join(MCPP_HOME, "registry", "data", "xpkgs")

local function first_dir(base)
    if not os.isdir(base) then return nil end
    local dirs = os.dirs(path.join(base, "*"))
    table.sort(dirs)
    return dirs[#dirs]
end

-- The compiler VERSION must come from mcpp.toml, not from "newest directory
-- wins": the registry holds several GCCs (15.1.0 and 16.1.0 here) and picking
-- the lexically-last one only happens to agree with the pin. A benchmark whose
-- fairness rests on a coincidence is not a benchmark.
--
-- The pin is read inside on_load below, not here: xmake's DESCRIPTION scope has
-- no `io`, so reading a file at this level dies with "attempt to index a nil
-- value (global 'io')" and takes every target in the project down with it.
local GCC_ROOT     = path.join(XPKGS, "xim-x-gcc")
local BINUTILS_DIR = first_dir(path.join(XPKGS, "xim-x-binutils"))
local GCC_DIR      = first_dir(GCC_ROOT)   -- fallback; on_load narrows it to the pin
local SYSROOT      = path.join(MCPP_HOME, "registry", "subos", "default")

-- mcpp.toml pins mcpplibs.cmdline = "0.0.1" exactly; newer versions may also be
-- unpacked in the registry, so pin rather than take the newest or the two builds
-- would not be compiling the same code.
local CMDLINE_VER = "0.0.1"
local CMDLINE_SRC = path.join(XPKGS, "mcpplibs-x-cmdline", CMDLINE_VER,
                              "cmdline-" .. CMDLINE_VER, "src")

option("pin_payload")
    set_default(true)
    set_showmenu(true)
    set_description("Pin the hermetic mcpp GCC payload (required for a fair benchmark)")
option_end()

if GCC_DIR and BINUTILS_DIR then
    toolchain("mcpp-gcc")
        set_kind("standalone")
        set_homepage("hermetic gcc payload resolved by mcpp")
        set_toolset("cc",  path.join(GCC_DIR, "bin", "gcc"))
        set_toolset("cxx", path.join(GCC_DIR, "bin", "g++"))
        set_toolset("ld",  path.join(GCC_DIR, "bin", "g++"))
        set_toolset("sh",  path.join(GCC_DIR, "bin", "g++"))
        set_toolset("ar",  path.join(BINUTILS_DIR, "bin", "ar"))
        set_toolset("strip", path.join(BINUTILS_DIR, "bin", "strip"))
        on_load(function (toolchain)
            -- Narrow the compiler to the version mcpp.toml pins, so both arms of
            -- the benchmark run the same binary by construction rather than by
            -- luck of directory ordering.
            local manifest = path.join(MCPP_ROOT, "mcpp.toml")
            if os.isfile(manifest) then
                local in_toolchain = false
                for _, line in ipairs((io.readfile(manifest) or ""):split("\n", {plain = true})) do
                    local section = line:match("^%s*%[(.-)%]")
                    if section then in_toolchain = (section == "toolchain") end
                    if in_toolchain then
                        local fam, ver = line:match('^%s*default%s*=%s*"([%w_]+)@([%w%.%-]+)"')
                        if fam == "gcc" and ver then
                            local pinned = path.join(XPKGS, "xim-x-gcc", ver)
                            if os.isdir(pinned) then
                                toolchain:set("toolset", "cc",  path.join(pinned, "bin", "gcc"))
                                toolchain:set("toolset", "cxx", path.join(pinned, "bin", "g++"))
                                toolchain:set("toolset", "ld",  path.join(pinned, "bin", "g++"))
                                toolchain:set("toolset", "sh",  path.join(pinned, "bin", "g++"))
                            else
                                utils.warning("mcpp.toml pins gcc@%s, absent from the registry; "
                                              .. "benchmark comparability is void", ver)
                            end
                            break
                        end
                    end
                end
            end
            -- -B must reach BOTH compile and link: the driver spawns `as` from it
            -- at compile time and `ld` from it at link time. Omitting it on either
            -- side silently falls through to PATH — where, on this host, the
            -- xlings `as` shim resolves to a stale path and every compile dies.
            toolchain:add("cxflags", "-B" .. path.join(BINUTILS_DIR, "bin"), {force = true})
            toolchain:add("ldflags", "-B" .. path.join(BINUTILS_DIR, "bin"), {force = true})
            if os.isdir(SYSROOT) then
                toolchain:add("cxflags", "--sysroot=" .. SYSROOT, {force = true})
                toolchain:add("ldflags", "--sysroot=" .. SYSROOT, {force = true})
            end
        end)
    toolchain_end()
end

-- ---------------------------------------------------------------------------
-- The one and only target: mcpp's CLI binary.
-- ---------------------------------------------------------------------------
target("mcpp")
    set_kind("binary")

    -- Source set == mcpp.toml's inferred glob src/**/*.{cppm,cpp}. mcpp infers
    -- kind=bin from src/main.cpp; xmake needs it spelled out.
    add_files(path.join(MCPP_ROOT, "src/**.cppm"))
    add_files(path.join(MCPP_ROOT, "src/main.cpp"))

    -- mcpp.toml: include_dirs = ["src/libs/json"] — src/libs/json.cppm reaches
    -- for <json.hpp> from its global module fragment.
    add_includedirs(path.join(MCPP_ROOT, "src/libs/json"))

    -- mcpp.toml: [dependencies] mcpplibs.cmdline = "0.0.1".
    -- mcpp stages prebuilt objects for this out of its global build cache; xmake
    -- has no such cache, so it compiles the 3 units from source. That is a ~1s
    -- handicap on xmake's cold build and is called out in the benchmark report
    -- rather than hidden.
    if os.isdir(CMDLINE_SRC) then
        add_files(path.join(CMDLINE_SRC, "*.cppm"))
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

    -- Pin the payload only when the caller did NOT ask for a specific toolchain.
    -- An unconditional set_toolchains() here SILENTLY OVERRIDES `xmake f
    -- --toolchain=llvm`: the benchmark then reports a "clang" cell that was in
    -- fact compiled by g++, and the giveaway is only that the number lands
    -- suspiciously close to the gcc one. Always verify with
    --     xmake show -t mcpp | grep 'compiler (cxx)'
    local requested = get_config("toolchain")
    if has_config("pin_payload") and GCC_DIR
       and (requested == nil or requested == "" or requested == "mcpp-gcc") then
        set_toolchains("mcpp-gcc")
    end
target_end()
