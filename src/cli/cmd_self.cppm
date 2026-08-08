// mcpp.cli.cmd_self — CLI parsing + routing for the `mcpp self` family,
// doctor, why, env and explain. Implementations live in mcpp.doctor.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_self;

import std;
import mcpplibs.cmdline;
import mcpp.doctor;
import mcpp.toolchain.fingerprint;   // MCPP_VERSION
import mcpp.home;
import mcpp.platform;
import mcpp.wire;
import mcpp.libs.json;

namespace mcpp::cli {

// `self env --format json`.
//
// Deliberately NOT `env_report()` plus a serializer. That path calls
// `config::load_or_init()`, which on a machine that has never run mcpp
// creates $MCPP_HOME -- measured: `config.toml`, `registry/`, `cache/`,
// `bin/`, `build-cache/`, `log/`, six entries, where `mcpp --version`,
// `xpkg parse` and `cache list` create none. Reporting where things ARE
// should not be what puts them there.
//
// So the machine path computes paths and reads what already exists. When
// nothing does, it says so (`initialized: false`) and reports the paths mcpp
// WOULD use, rather than creating them to be able to answer. A client asking
// "where is your home" on a fresh machine gets an answer and an untouched
// disk.
//
// The human path is unchanged: someone typing `mcpp self env` at a prompt
// expects mcpp to set itself up, and always has.
namespace {

nlohmann::json env_data_readonly() {
    namespace fs = std::filesystem;
    const auto home = mcpp::home::root();
    std::error_code ec;

    const auto registry = home / "registry";
    const auto config   = home / "config.toml";
    const bool initialized = fs::exists(config, ec);

    auto s = [](const fs::path& p) { return p.string(); };
    return nlohmann::json{
        {"initialized",  initialized},
        {"mcppHome",     s(home)},
        {"registry",     s(registry)},
        {"xlingsHome",   s(registry)},          // registryDir unless overridden
        {"xlingsBinary", s(registry / "bin" /
                           ("xlings" + std::string(mcpp::platform::exe_suffix)))},
        {"config",       s(config)},
        {"buildCache",   s(mcpp::home::cache_root())},
        {"mcppVersion",  std::string(mcpp::toolchain::MCPP_VERSION)},
    };
}

}  // namespace

export int cmd_env(const mcpplibs::cmdline::ParsedArgs& parsed) {
    if (auto f = parsed.value("format")) {
        auto fmt = mcpp::wire::parse_format(*f);
        if (!fmt) {
            std::println(stderr, "error: {}",
                         mcpp::wire::unsupported_format(*f));
            return 2;
        }
        mcpp::wire::emit({
            .kind    = "mcpp.env",
            .effects = {},          // this path creates nothing; see above
            .data    = env_data_readonly(),
        });
        return 0;
    }
    return mcpp::doctor::env_report();
}

export int cmd_doctor(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::doctor::doctor_report();
}

export int cmd_why(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::doctor::why_report(parsed.positional(0));
}

// Also called directly by the dispatcher for the legacy `--explain CODE` form.
export int cmd_explain(std::string_view code) {
    return mcpp::doctor::explain_code(code);
}

export int cmd_self_version(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    std::println("mcpp {}", mcpp::toolchain::MCPP_VERSION);
    return 0;
}

export int cmd_self_init(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::doctor::self_init(parsed.is_flag_set("force"));
}

export int cmd_self_config(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::doctor::self_config(parsed.option_or_empty("mirror").value());
}

// Used both by `mcpp explain <CODE>` (top-level) and `mcpp self explain
// <CODE>` (legacy alias).
export int cmd_explain_action(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string code = parsed.positional(0);
    if (code.empty()) {
        std::println(stderr, "error: explain requires an error code (e.g. E0001)");
        return 2;
    }
    return cmd_explain(code);
}

} // namespace mcpp::cli
