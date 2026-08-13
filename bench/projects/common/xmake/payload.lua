-- Shared by every xmake arm in bench/projects/.
--
-- WHY THIS IS SHARED. mcpp's arm and xlings' arm need the same thing — "make
-- xmake drive the same process tree mcpp does" — and had two copies of a
-- 60-line toolchain block. Two copies of a toolchain definition is the worst
-- place for a copy: they drift by one flag and the benchmark reports the
-- difference between the two DESCRIPTIONS as an engine result.
--
-- THE COMPILER IS AN AXIS, so this defines one toolchain per family rather than
-- one block of flags:
--
--   mcpp-gcc    the registry's gcc + its binutils + the subos sysroot
--   mcpp-clang  the registry's clang + its own include chain (NOT --sysroot;
--               see the comment there — handing clang gcc's payload puts the
--               two arms on different libc)
--   msvc        the SYSTEM Visual Studio, which is what mcpp uses too
--               (`msvc@system`), so there is nothing to define
--
-- ⚠️ TWO SCOPES, TWO DIFFERENT MISSING PIECES. xmake's DESCRIPTION scope (the
-- top level of an xmake.lua, and this file) has no `io`. `on_load`'s sandbox has
-- `io` but cannot see globals defined here. Both were hit, in that order, trying
-- to share the manifest reader — see the note inside bench_define_toolchains.
-- Anything that reads a file must live inside on_load; anything that only
-- touches `os`/`path` can live out here.
--
-- Usage, from a project's xmake.lua:
--     includes("../common/xmake/payload.lua")
--     bench_define_toolchains(path_to_the_measured_tree_mcpp_toml)
--     ...
--     local tc = bench_pinned_toolchain(); if tc then set_toolchains(tc) end

function bench_mcpp_home()
    local home = os.getenv("MCPP_HOME")
    if not home then
        local base = os.getenv("HOME") or os.getenv("USERPROFILE") or ""
        home = path.join(base, ".mcpp")
    end
    return home
end

function bench_xpkgs()
    return path.join(bench_mcpp_home(), "registry", "data", "xpkgs")
end

function bench_sysroot()
    return path.join(bench_mcpp_home(), "registry", "subos", "default")
end

-- Newest unpacked version of a package. For PAYLOAD components only — never for
-- a dependency whose version the manifest pins. The registry holds several
-- versions, and "lexically last" agreeing with the pin is a coincidence; a
-- benchmark whose fairness rests on a coincidence is not a benchmark.
function bench_newest(name)
    local base = path.join(bench_xpkgs(), name)
    if not os.isdir(base) then return nil end
    local dirs = os.dirs(path.join(base, "*"))
    if #dirs == 0 then return nil end
    table.sort(dirs)
    return dirs[#dirs]
end

-- The single subdirectory a source package unpacks into
-- (`mcpplibs-x-cmdline/0.0.2/cmdline-0.0.2/`). Discovered rather than composed
-- from `<name>-<version>`: `mcpplibs.capi-x-lua/0.0.3/` does not follow that
-- pattern, and a guess there finds nothing — which surfaces as a missing module
-- three files later, not as a missing path.
function bench_package_root(name, version)
    local base = path.join(bench_xpkgs(), name, version)
    if not os.isdir(base) then return nil end
    local dirs = os.dirs(path.join(base, "*"))
    if #dirs == 0 then return nil end
    table.sort(dirs)
    return dirs[1]
end

-- Defines `mcpp-gcc` and `mcpp-clang` when their payloads are present.
--
-- `manifest` is the MEASURED TREE's mcpp.toml. Its `[toolchain] default` pins
-- the exact version, narrowed inside on_load so both arms run the same binary by
-- construction rather than by luck of directory ordering.
function bench_define_toolchains(manifest)
    local xpkgs     = bench_xpkgs()
    local sysroot   = bench_sysroot()
    local binutils  = bench_newest("xim-x-binutils")
    local gcc_dir   = bench_newest("xim-x-gcc")
    local llvm_dir  = bench_newest("xim-x-llvm")
    -- Resolved HERE and captured as upvalues, because on_load cannot call
    -- bench_newest: its sandbox does not see this file's globals.
    local glibc_dir = bench_newest("xim-x-glibc")
    local uapi_dir  = bench_newest("xim-x-linux-headers")

    -- ⚠️ THE MANIFEST READER IS WRITTEN OUT INSIDE EACH on_load, TWICE.
    --
    -- Not an oversight — it is the only scope it can live in:
    --   * defined out here, a Lua closure carries its DEFINITION environment, so
    --     it resolves `io` against the description scope, where io is nil:
    --         attempt to index a nil value (global 'io')
    --   * defined as a global in this file, on_load's sandbox cannot see it:
    --         attempt to call a nil value (global 'bench_manifest_toolchain')
    -- Both were hit, in that order, trying to share it. Twelve lines twice
    -- inside ONE file is a far weaker coupling than the 60-line toolchain block
    -- that used to be copied across two project files — which is what this
    -- module exists to remove.

    if gcc_dir and binutils then
        toolchain("mcpp-gcc")
            set_kind("standalone")
            set_homepage("hermetic gcc payload resolved by mcpp")
            set_toolset("cc",    path.join(gcc_dir, "bin", "gcc"))
            set_toolset("cxx",   path.join(gcc_dir, "bin", "g++"))
            set_toolset("ld",    path.join(gcc_dir, "bin", "g++"))
            set_toolset("sh",    path.join(gcc_dir, "bin", "g++"))
            set_toolset("ar",    path.join(binutils, "bin", "ar"))
            set_toolset("strip", path.join(binutils, "bin", "strip"))
            on_load(function (toolchain)
                local read_pin = function (m)
                    if not m or not os.isfile(m) then return nil end
                    local in_tc = false
                    for _, line in ipairs((io.readfile(m) or ""):split("\n", {plain = true})) do
                        local section = line:match("^%s*%[(.-)%]")
                        if section then in_tc = (section == "toolchain") end
                        if in_tc then
                            local f, v = line:match('^%s*default%s*=%s*"([%w_]+)@([%w%.%-]+)"')
                            if f and v then return f, v end
                        end
                    end
                    return nil
                end
                local fam, ver = read_pin(manifest)
                if fam == "gcc" and ver then
                    local pinned = path.join(xpkgs, "xim-x-gcc", ver)
                    if os.isdir(pinned) then
                        toolchain:set("toolset", "cc", path.join(pinned, "bin", "gcc"))
                        for _, k in ipairs({"cxx", "ld", "sh"}) do
                            toolchain:set("toolset", k, path.join(pinned, "bin", "g++"))
                        end
                    else
                        utils.warning("mcpp.toml pins gcc@%s, absent from the registry; "
                                      .. "benchmark comparability is void", ver)
                    end
                end
                -- -B must reach BOTH compile and link: the driver spawns `as`
                -- from it at compile time and `ld` from it at link time.
                -- Omitting it on either side silently falls through to PATH —
                -- where, on a host with xlings shims, `as` can resolve to a
                -- stale path and every compile dies.
                toolchain:add("cxflags", "-B" .. path.join(binutils, "bin"), {force = true})
                toolchain:add("ldflags", "-B" .. path.join(binutils, "bin"), {force = true})
                if os.isdir(sysroot) then
                    toolchain:add("cxflags", "--sysroot=" .. sysroot, {force = true})
                    toolchain:add("ldflags", "--sysroot=" .. sysroot, {force = true})
                end
            end)
        toolchain_end()
    end

    if llvm_dir then
        toolchain("mcpp-clang")
            set_kind("standalone")
            set_homepage("hermetic llvm payload resolved by mcpp")
            set_toolset("cc",    path.join(llvm_dir, "bin", "clang"))
            set_toolset("cxx",   path.join(llvm_dir, "bin", "clang++"))
            set_toolset("ld",    path.join(llvm_dir, "bin", "clang++"))
            set_toolset("sh",    path.join(llvm_dir, "bin", "clang++"))
            set_toolset("ar",    path.join(llvm_dir, "bin", "llvm-ar"))
            set_toolset("strip", path.join(llvm_dir, "bin", "llvm-strip"))
            -- xmake finds libc++'s `std.cppm` through the SDK dir, and it reads
            -- that at DESCRIPTION scope — setting it inside on_load is too late
            -- and leaves `std and std.compat modules not found!`, after which
            -- `build.c++.modules.std` degrades silently and the arm measures a
            -- project that does not use `import std;` against one that does.
            set_sdkdir(llvm_dir)
            on_load(function (toolchain)
                local read_pin = function (m)
                    if not m or not os.isfile(m) then return nil end
                    local in_tc = false
                    for _, line in ipairs((io.readfile(m) or ""):split("\n", {plain = true})) do
                        local section = line:match("^%s*%[(.-)%]")
                        if section then in_tc = (section == "toolchain") end
                        if in_tc then
                            local f, v = line:match('^%s*default%s*=%s*"([%w_]+)@([%w%.%-]+)"')
                            if f and v then return f, v end
                        end
                    end
                    return nil
                end
                local root = llvm_dir
                local fam, ver = read_pin(manifest)
                if fam == "llvm" and ver then
                    local pinned = path.join(xpkgs, "xim-x-llvm", ver)
                    if os.isdir(pinned) then
                        root = pinned
                        toolchain:set("toolset", "cc", path.join(pinned, "bin", "clang"))
                        for _, k in ipairs({"cxx", "ld", "sh"}) do
                            toolchain:set("toolset", k, path.join(pinned, "bin", "clang++"))
                        end
                    else
                        utils.warning("mcpp.toml pins llvm@%s, absent from the registry; "
                                      .. "benchmark comparability is void", ver)
                    end
                end
                -- xmake locates libc++'s `std.cppm` through the LLVM SDK dir,
                -- not through the include chain below. Without it:
                --     warning: std and std.compat modules not found!
                -- and `set_policy("build.c++.modules.std", true)` silently
                -- degrades — the arm then measures a project that does not use
                -- `import std;` against one that does.
                toolchain:set("sdkdir", root)

                -- NOT --sysroot. mcpp drives clang with an explicit include
                -- chain instead (verified against a real mcpp compile command),
                -- and handing clang gcc's sysroot puts the two arms on different
                -- libc — the same class of bug the CMake side documents, where
                -- the error names a struct field in <stdio.h> and neither the
                -- flag nor the target that is wrong.
                if os.isdir(path.join(root, "include", "c++", "v1")) then
                    toolchain:add("cxflags", "--no-default-config", "-nostdinc++", {force = true})
                    toolchain:add("cxflags", "-isystem" .. path.join(root, "include", "c++", "v1"),
                                  {force = true})
                    -- The per-triple directory carries __config_site; it is
                    -- absent on some builds, so it is added only when present.
                    for _, d in ipairs(os.dirs(path.join(root, "include", "*", "c++", "v1"))) do
                        toolchain:add("cxflags", "-isystem" .. d, {force = true})
                    end
                    toolchain:add("ldflags", "-nostdlib++", "-L" .. path.join(root, "lib"),
                                  "-lc++", "-lc++abi", {force = true})
                end
                if glibc_dir and os.isdir(path.join(glibc_dir, "include")) then
                    toolchain:add("cxflags", "-isystem" .. path.join(glibc_dir, "include"),
                                  {force = true})
                end
                if uapi_dir and os.isdir(path.join(uapi_dir, "include")) then
                    toolchain:add("cxflags", "-isystem" .. path.join(uapi_dir, "include"),
                                  {force = true})
                end
            end)
        toolchain_end()
    end
end

-- Which toolchain a target should pin, given what the caller asked for.
--
-- Description-scope safe: it reads no files (see the scope note at the top).
-- The FAMILY is decided here; the exact VERSION is narrowed in on_load.
--
-- Returns nil when the caller named a non-payload toolchain — an unconditional
-- set_toolchains() SILENTLY OVERRIDES `xmake f --toolchain=llvm`: the benchmark
-- then reports a "clang" cell that was in fact compiled by g++, and the giveaway
-- is only that the number lands suspiciously close to the gcc one. Verify with
--     xmake show -t <target> | grep 'compiler (cxx)'
--
-- To measure the clang cell, ask for the payload by name:
--     xmake f --toolchain=mcpp-clang
function bench_pinned_toolchain()
    local requested = get_config("toolchain")
    if requested ~= nil and requested ~= "" then
        if requested:startswith("mcpp-") then return requested end
        return nil
    end
    if bench_newest("xim-x-gcc")  then return "mcpp-gcc"  end
    if bench_newest("xim-x-llvm") then return "mcpp-clang" end
    return nil
end
