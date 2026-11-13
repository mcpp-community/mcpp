// A project's local path index and a home's own index: namespace-aware file names, the name a
// cached dependency is reported by, a payload directory without its marker reinstalled, and a
// custom namespace filed under a non-canonical file name (formerly tests/e2e/52, 53, 60, 76).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

using env_list = std::vector<std::pair<std::string, std::optional<std::string>>>;

// The home of the case over the machine's payloads (_inherit_toolchain.sh), with `more` set.
s::options inherited(env_list more = {}) { return {.where = s::home::inherited, .env = std::move(more)}; }

// `[xlings] binary` and `home` in the home's config.toml, as the scripts wrote it.
void fake_xlings_config(const fs::path& home, const fs::path& binary, const fs::path& xlings_home) {
    s::project(home, {{"config.toml", std::format(R"([xlings]
binary = "{}"
home = "{}"

[index]
default = "mcpplibs"

[index.repos."mcpplibs"]
url = "https://github.com/mcpp-community/mcpp-index.git"

[cache]
search_ttl_seconds = 3600

[build]
default_jobs = 0

[toolchain]
default = "gcc@16.1.0"
)", binary.generic_string(), xlings_home.generic_string())}});
}

constexpr std::string_view handshake = R"HS([[ "${1:-}" == "interface" && "${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.13","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index","payload_currency","binding_state","network_diagnostics","download_socket_budgets","read_only_queries"]}'; exit 0; })HS";

std::string lua_package(std::string_view name, std::string_view version, std::string_view description, std::string_view mcpp_segment) {
    return std::format(R"(package = {{
    spec = "1",
    namespace = "compat",
    name = "{0}",
    description = "{2}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux = {{
            ["{1}"] = {{
                url = "https://example.invalid/{0}-{1}.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            }},
        }},
    }},{3}
}}
)", name, version, description, mcpp_segment);
}

std::string c_lib_segment(std::string_view source, std::string_view target) {
    return std::format(R"(
    mcpp = {{
        language = "c++23",
        import_std = false,
        sources = {{ "{}" }},
        targets = {{ ["{}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},)", source, target);
}

E2E local{"local", [] {
    "52 local path namespaced index"_test[s::gcc][s::fresh_sandbox] = [] {
        // A local path index finds pkgs/c/compat.cfg.lua; the project builds and runs. A clean
        // project installs it through the NDJSON interface with the qualified name
        // compat:compat.cfg@1.0.0, refreshing no builtin index and writing no xim index.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        const auto seed = home / "registry/subos/default/.xlings.json";
        expect(fatal(fs::is_regular_file(seed))) << "inherited toolchain has no default SubOS runtime contract";
        s::project index(tmp / "local-index", {{"pkgs/c/compat.cfg.lua", lua_package("compat.cfg", "1.0.0", "Namespaced local path index package", c_lib_segment("src/*.c", "cfg"))}});
        const auto indices = std::format("[indices]\ncompat = {{ path = \"{}\" }}\n\n[dependencies.compat]\ncfg = \"1.0.0\"\n", index.dir().generic_string());
        s::project app(tmp / "project/app", {
            {".mcpp/.xlings/data/xpkgs/compat.cfg/1.0.0/src/cfg.c", "int cfg_value(void) {\n    return 42;\n}\n"},
            {"src/main.cpp", "extern \"C\" int cfg_value(void);\nint main() {\n    return cfg_value() == 42 ? 0 : 1;\n}\n"},
            {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n" + indices + "\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        auto r = app.mcpp({"build"}, inherited());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = app.mcpp({"run"}, inherited());
        expect(fatal(e2e::succeeded(r))) << s::text(r);

        const auto fake_registry = tmp / "fake-registry";
        s::project(fake_registry / "data", {{"xim-pkgindex/pkgs/p/python.lua", R"(package = {
    spec = "1",
    name = "python",
    xpm = {
        linux = {
            ["3"] = {
                url = "https://example.invalid/python.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
)"}});
        if (fs::is_directory(s::shared_home() / "registry/data/xpkgs"))
            fs::create_directory_symlink(s::shared_home() / "registry/data/xpkgs", fake_registry / "data/xpkgs");
        s::write_executable(tmp / "fake-bin/xlings", std::string("#!/usr/bin/env bash\n# The handshake mcpp asks of every xlings (docs/25 §5).\n") + std::string(handshake) + R"SH(
set -e
if [[ "${1:-}" == "self" && "${2:-}" == "init" ]]; then
    mkdir -p "${XLINGS_HOME:?}/subos/default"
    cp "${FAKE_XLINGS_SUBOS_INFO:?}" \
       "$XLINGS_HOME/subos/default/.xlings.json"
    exit 0
fi
if [[ "${1:-}" == "update" || ( "${1:-}" == "interface" && "${2:-}" == "update_packages" ) ]]; then
    printf 'update\n' > "${FAKE_XLINGS_UPDATE_LOG:?}"
    exit 0
fi
if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--args" ]]; then
            printf '%s\n' "$2" > "${FAKE_XLINGS_LOG:?}"
            break
        fi
        shift
    done
    if grep -q '"name": "xim"' "${XLINGS_PROJECT_DIR:?}/.xlings.json"; then
        echo "official xim index must NOT be injected into project .xlings.json" >&2
        cat "${XLINGS_PROJECT_DIR:?}/.xlings.json" >&2 2>/dev/null || true
        exit 24
    fi
    if [[ ! -d "${XLINGS_PROJECT_DIR:?}/.xlings/data/compat/pkgs" \
       && ! -d "${XLINGS_PROJECT_DIR:?}/data/compat/pkgs" ]]; then
        echo "missing project local path index link" >&2
        find "${XLINGS_PROJECT_DIR:?}" -maxdepth 4 -type d -print >&2 2>/dev/null || true
        exit 23
    fi
    install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.cfg/1.0.0"
    mkdir -p "$install_root/src"
    cat > "$install_root/src/cfg.c" <<'SRC'
int cfg_value(void) {
    return 42;
}
SRC
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.1,"files":[{"name":"compat:compat.cfg@1.0.0","downloadedBytes":0,"totalBytes":0,"started":true,"finished":false}]}}\n'
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.3,"files":[{"name":"compat:compat.cfg@1.0.0","downloadedBytes":1024,"totalBytes":1024,"started":true,"finished":true}]}}\n'
    printf '{"kind":"result","exitCode":0}\n'
    exit 0
fi
if [[ "${1:-}" == "install" ]]; then
    printf '%s\n' "$*" > "${FAKE_XLINGS_DIRECT_LOG:?}"
    exit 0
fi
exit 0
)SH");
        fake_xlings_config(home, tmp / "fake-bin/xlings", fake_registry);
        s::project clean(tmp / "project/clean", {
            {"src/clean.cpp", "extern \"C\" int cfg_value(void);\nint clean_value(void) {\n    return cfg_value();\n}\n"},
            {"mcpp.toml", "[package]\nname = \"clean\"\nversion = \"0.1.0\"\n\n" + indices + "\n[targets.clean]\nkind = \"lib\"\n"}});
        const auto log = tmp / "fake-xlings.log", direct = tmp / "fake-xlings-direct.log", update = tmp / "fake-xlings-update.log";
        r = clean.mcpp({"build"}, inherited({{"FAKE_XLINGS_LOG", log.string()}, {"FAKE_XLINGS_DIRECT_LOG", direct.string()},
                                             {"FAKE_XLINGS_UPDATE_LOG", update.string()}, {"FAKE_XLINGS_SUBOS_INFO", seed.string()}}));
        expect(fatal(e2e::succeeded(r))) << "clean local path dependency install failed: " << s::text(r);
        expect(fatal(!fs::exists(update))) << "a local path index dependency should not refresh the builtin package index: " << s::text(r);
        expect(fatal(!fs::exists(direct))) << "a project local path install should use the NDJSON interface, not direct xlings install: " << (fs::exists(direct) ? s::read_file(direct) : "");
        expect(fatal(fs::exists(log) && e2e::contains(s::read_file(log), "compat:compat.cfg@1.0.0"))) << "the install target should name the full package: " << (fs::exists(log) ? s::read_file(log) : "(nothing recorded)") << "\n" << s::text(r);
    };

    "53 namespaced cache label"_test[s::gcc][s::fresh_sandbox] = [] {
        // A local path index dependency refreshes no builtin index and shows no xlings update
        // output; rebuilt from the cache, it is reported as compat.widget.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        s::project index(tmp / "local-index", {{"pkgs/c/compat.widget.lua", lua_package("compat.widget", "1.0.0", "Namespaced cache label package", "")}});
        const auto pkg = std::string(".mcpp/.xlings/data/xpkgs/compat.widget/1.0.0/widget-1.0.0/");
        s::project app(tmp / "project/app", {
            {pkg + "mcpp.toml", "[package]\nnamespace = \"compat\"\nname = \"widget\"\nversion = \"1.0.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.widget]\nkind = \"lib\"\n"},
            {pkg + "src/widget.c", "int widget_value(void) {\n    return 7;\n}\n"},
            {"src/main.cpp", "extern \"C\" int widget_value(void);\nint main() {\n    return widget_value() == 7 ? 0 : 1;\n}\n"},
            {"mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ncompat = {{ path = \"{}\" }}\n\n[dependencies.compat]\nwidget = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", index.dir().generic_string())}});
        auto r = app.mcpp({"build"}, inherited());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"(^\[[0-9]+/[0-9]+\] )"))) << "automatic index refresh should not expose xlings update output: " << s::text(r);
        expect(fatal(!fs::exists(home / "indices/mcpplibs/.mcpp-index-updated"))) << "a local path index dependency should not refresh the builtin mcpplibs index: " << s::text(r);
        fs::remove_all(app.at("target"));
        r = app.mcpp({"build", "-v"}, inherited());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached compat\.widget v1\.0\.0 \((index [^,)]+, )?[0-9]+ units?\))"))) << "the cached namespaced dependency should be reported as compat.widget: " << s::text(r);
    };

    "60 stale xpkg cache reinstall"_test[s::gcc][s::fresh_sandbox] = [] {
        // A payload directory without its marker is installed again (through the interface,
        // never direct), mcpp writes no marker into the payload, and the program runs on the
        // reinstalled source.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        s::project index(tmp / "local-index", {{"pkgs/c/compat.stale.lua", lua_package("compat.stale", "1.0.0", "stale cache reinstall test package", c_lib_segment("src/stale.c", "stale"))}});
        s::write_executable(tmp / "fake-bin/xlings", std::string("#!/usr/bin/env bash\n") + std::string(handshake) + R"SH(
set -e
if [[ "${1:-}" == "self" && "${2:-}" == "init" ]]; then
    mkdir -p "${XLINGS_HOME:?}/subos/default"
    printf '{}\n' > "$XLINGS_HOME/subos/default/.xlings.json"
    exit 0
fi
if [[ "${1:-}" == "update" || ( "${1:-}" == "interface" && "${2:-}" == "update_packages" ) ]]; then
    exit 0
fi
if [[ "${1:-}" == "interface" && "${2:-}" == "install_packages" ]]; then
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--args" ]]; then printf '%s\n' "$2" > "${FAKE_INSTALL_LOG:?}"; break; fi
        shift
    done
    install_root="${XLINGS_PROJECT_DIR:?}/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0"
    mkdir -p "$install_root/src"
    cat > "$install_root/src/stale.c" <<'SRC'
int stale_value(void) {
    return 42;
}
SRC
    printf '{"kind":"data","dataKind":"download_progress","payload":{"elapsedSec":0.2,"files":[{"name":"compat:compat.stale@1.0.0","downloadedBytes":2048,"totalBytes":2048,"started":true,"finished":true}]}}\n'
    printf '{"kind":"result","exitCode":0}\n'
    exit 0
fi
if [[ "${1:-}" == "install" ]]; then
    echo "direct install should not be used for project path indices" >&2
    exit 31
fi
exit 0
)SH");
        fake_xlings_config(home, tmp / "fake-bin/xlings", home / "registry");
        const std::string payload = ".mcpp/.xlings/data/xpkgs/compat-x-compat.stale/1.0.0";
        s::project app(tmp / "project/app", {
            {payload + "/stale-1.0.0/src/stale.c", "int stale_value(void) {\n    return 13;\n}\n"},
            {"src/main.cpp", "extern \"C\" int stale_value(void);\nint main() {\n    return stale_value() == 42 ? 0 : 1;\n}\n"},
            {"mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ncompat = {{ path = \"{}\" }}\n\n[dependencies.compat]\nstale = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", index.dir().generic_string())}});
        const auto log = tmp / "fake-install.log";
        auto r = app.mcpp({"build"}, inherited({{"FAKE_INSTALL_LOG", log.string()}}));
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(fs::exists(log) && e2e::contains(s::read_file(log), "compat:compat.stale@1.0.0"))) << "a stale unmarked xpkg directory should trigger reinstall: " << s::text(r);
        expect(fatal(!app.exists(payload + "/.mcpp_ok"))) << "mcpp wrote a marker into the payload";
        r = app.mcpp({"run"}, inherited({{"FAKE_INSTALL_LOG", log.string()}}));
        expect(fatal(e2e::succeeded(r))) << s::text(r);
    };

    "76 qualified custom ns noncanonical filename"_test[s::gcc][s::fresh_sandbox] = [] {
        // aimol.tensorvia-cpu, filed as pkgs/t/tensorvia-cpu.lua in the home's mcpplibs index,
        // resolves by its identity and is locked under namespace aimol (no mcpplibs.aimol);
        // mcpplibs.tensorvia-cpu does not resolve.
        const auto home = s::case_home(s::home::inherited);
        s::project(home / "indices/mcpplibs", {{".mcpp-index-updated", "ok\n"}, {"pkgs/t/tensorvia-cpu.lua", R"(package = {
    spec = "1",
    namespace = "aimol",
    name = "tensorvia-cpu",
    description = "Custom-namespace package filed under a non-canonical filename",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.1.1"] = {
                url = "https://example.invalid/tensorvia-cpu-0.1.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/tensorvia.cppm" },
        targets = { ["tensorvia-cpu"] = { kind = "lib" } },
        deps = {},
    },
}
)"}});
        s::project(home / "registry/data/xpkgs/aimol-x-tensorvia-cpu/0.1.1", {
            {"src/tensorvia.cppm", "export module tensorvia.cpu;\nexport int tensorvia_value() {\n    return 42;\n}\n"}, {".mcpp_ok", "ok\n"}});
        const auto manifest = [](std::string_view dep) {
            return std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n{} = \"0.1.1\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", dep);
        };
        s::project app(s::case_dir() / "project/app", {{"src/main.cpp", "import tensorvia.cpu;\nint main() {\n    return tensorvia_value() == 42 ? 0 : 1;\n}\n"},
                                                       {"mcpp.toml", manifest("aimol.tensorvia-cpu")}});
        auto r = app.mcpp({"build"}, inherited());
        expect(fatal(e2e::succeeded(r))) << "aimol.tensorvia-cpu did not resolve (identity-first regression): " << s::text(r);
        r = app.mcpp({"run"}, inherited());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto lock = app.read("mcpp.lock");
        expect(fatal(e2e::contains(lock, "namespace = \"aimol\""))) << "the lock must record the resolved namespace aimol: " << lock;
        expect(fatal(s::lacks(lock, "mcpplibs.aimol"))) << "the front candidate mcpplibs.aimol leaked into the lock: " << lock;
        app.write("mcpp.toml", manifest("mcpplibs.tensorvia-cpu"));
        app.remove("mcpp.lock");
        r = app.mcpp({"build"}, inherited());
        expect(fatal(r.started && r.status != 0)) << "mcpplibs.tensorvia-cpu must NOT resolve (the package is aimol-namespaced): " << s::text(r);
    };
}};

}  // namespace
