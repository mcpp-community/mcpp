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

local XLINGS_ROOT = os.getenv("XLINGS_ROOT")
local XLINGS_MANIFEST = XLINGS_ROOT and path.join(XLINGS_ROOT, "mcpp.toml")

option("pin_payload")
    set_default(true)
    set_showmenu(true)
    set_description("Pin the hermetic mcpp toolchain payload (required for a fair benchmark)")
option_end()

bench_define_toolchains(XLINGS_MANIFEST)

target("xlings")
    set_kind("binary")

    on_load(function (target)
        local root = os.getenv("XLINGS_ROOT")
        if not root or not os.isfile(path.join(root, "mcpp.toml")) then
            raise("set XLINGS_ROOT=<checkout of https://github.com/openxlings/xlings>; "
                  .. "this description is deliberately not vendored — see README.md")
        end

        -- Source set == xlings' mcpp.toml inferred glob src/**/*.{cppm,cpp};
        -- mcpp infers kind=bin from src/main.cpp, xmake needs it spelled out.
        target:add("files", path.join(root, "src/**.cppm"))
        target:add("files", path.join(root, "src/main.cpp"))

        -- `[build] include_dirs = ["src/libs/json"]` — src/libs/json.cppm reaches
        -- for <json.hpp> from its global module fragment.
        target:add("includedirs", path.join(root, "src/libs/json"))
        -- `[build] cxxflags`
        target:add("defines", "LIBARCHIVE_STATIC", "UNICODE", "_UNICODE")

        -- Source dependencies, PINNED to xlings' mcpp.toml. Newer versions are
        -- usually also unpacked in the registry, and taking the newest would
        -- mean the arms compile different code.
        --
        -- mcpp stages prebuilt objects for these out of its global build cache
        -- while xmake compiles them from source: a handicap on xmake's cold
        -- build, declared here rather than hidden.
        for _, dep in ipairs({{"mcpplibs-x-cmdline",   "0.0.2"},
                              {"mcpplibs-x-xpkg",      "0.0.57"},
                              {"mcpplibs-x-tinyhttps", "0.2.9"},
                              {"mcpplibs.capi-x-lua",  "0.0.3"}}) do
            local dir = bench_package_root(dep[1], dep[2])
            if dir and os.isdir(path.join(dir, "src")) then
                target:add("files", path.join(dir, "src/**.cppm"))
            else
                utils.warning("dependency %s %s is not unpacked in the registry; "
                              .. "this build will not match mcpp's own", dep[1], dep[2])
            end
        end

        -- Header-providing packages. Each unpacks ONE level below the version
        -- directory (`compat-x-ftxui/6.1.9/FTXUI-6.1.9/include`), so globbing
        -- `<ver>/include` finds nothing and the failure surfaces on the first
        -- importer rather than on the glob.
        --
        -- The list is TRANSITIVE and written out rather than discovered, because
        -- the discovery is what mcpp's package manager does: xlings names 6
        -- direct dependencies, and wiring the four source ones in surfaced two
        -- more (mbedtls for tinyhttps, lua for capi.lua).
        for _, pkg in ipairs({"compat-x-ftxui", "compat-x-libarchive",
                              "compat-x-mbedtls", "compat-x-lua"}) do
            for _, ver in ipairs(os.dirs(path.join(bench_xpkgs(), pkg, "*"))) do
                for _, inner in ipairs(os.dirs(path.join(ver, "*"))) do
                    for _, sub in ipairs({"include", "src", "libarchive"}) do
                        if os.isdir(path.join(inner, sub)) then
                            target:add("includedirs", path.join(inner, sub))
                        end
                    end
                end
            end
        end
    end)

    -- `mcpplibs.xpkg.lua_stdlib` is GENERATED by libxpkg's build.mcpp rather
    -- than checked in: it embeds every .lua under src/lua-stdlib as a string
    -- named after the file. Same RULE as embed_lua_stdlib.cmake (which the cmake
    -- arm runs) — deliberately not the same list, because a copied list here
    -- already drifted once and the failure landed three files away, in a
    -- consumer, as `'base64_lua' is not a member of ...detail`.
    --
    -- before_build rather than a custom rule: the file must exist before module
    -- dependency scanning, which runs ahead of any per-file rule.
    before_build(function (target)
        local pkg = bench_package_root("mcpplibs-x-xpkg", "0.0.57")
        if not pkg then return end
        local stdlib = path.join(pkg, "src", "lua-stdlib")
        if not os.isdir(stdlib) then return end

        local out = path.join(os.projectdir(), "build", "generated", "xpkg-lua-stdlib.cppm")
        local text = {
            "// Generated by bench/projects/xlings/xmake.lua — do not edit.",
            "// Mirrors what libxpkg's build.mcpp produces; edit the .lua sources.",
            "module;",
            "export module mcpplibs.xpkg.lua_stdlib;",
            "import std;",
            "",
            "export namespace mcpplibs::xpkg::detail {",
            "",
        }
        local files = os.files(path.join(stdlib, "**.lua"))
        if #files == 0 then
            raise("no .lua under %s — either the package layout changed or the "
                  .. "version pin is wrong. Emitting an empty module would fail "
                  .. "three files away, in a consumer.", stdlib)
        end
        table.sort(files)
        for _, f in ipairs(files) do
            local var = path.basename(f) .. "_lua"
            -- A raw string literal, so nothing in the Lua needs escaping. The
            -- delimiter is one no Lua file contains; if that stops being true
            -- the generated file will not compile, which is the loud failure.
            table.insert(text, ("inline const std::string_view %s = R\"XLUA(%s)XLUA\";")
                                   :format(var, io.readfile(f)))
            table.insert(text, "")
        end
        table.insert(text, "} // namespace mcpplibs::xpkg::detail")

        os.mkdir(path.directory(out))
        io.writefile(out, table.concat(text, "\n") .. "\n")
        target:add("files", out)
    end)

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
