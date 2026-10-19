// mcpp.cli.cmd_self — CLI parsing + routing for the `mcpp self` family,
// doctor, why, env and explain. Implementations live in mcpp.doctor.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_self;

import std;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.build.engine_package;
import mcpplibs.cmdline;
import mcpp.doctor;
import mcpp.toolchain.fingerprint;   // MCPP_VERSION
import mcpp.home;
import mcpp.platform;
import mcpp.toolchain.triple;          // pins::host_default_toolchain (WS8)
import mcpp.toolchain.msvc;            // msvc_available_here
import mcpp.wire;
import mcpp.libs.json;

namespace mcpp::cli {

// `self env --format json`.
//
// Both human and machine queries are read-only. This serializer reads paths
// and existing configuration directly, without toolchain detection; a cold home
// reports initialized=false and is left untouched.
namespace {

nlohmann::json env_data_readonly() {
    namespace fs = std::filesystem;
    const auto selected = mcpp::home::selection();
    const auto home = selected.effective;
    std::error_code ec;

    const auto registry = home / "registry";
    const auto config   = home / "config.toml";
    const bool initialized = fs::exists(config, ec);

    auto s = [](const fs::path& p) { return p.string(); };
    return nlohmann::json{
        {"initialized",  initialized},
        {"mcppHome",     s(home)},
        {"originalHome", mcpp::home::spelling(selected.original)},
        {"homeSource", selected.redirected ? "locator" : selected.source},
        {"homeLocator", selected.redirected ? mcpp::home::spelling(selected.original / "home.toml") : ""},
        {"registry",     s(registry)},
        {"xlingsHome",   s(registry)},          // registryDir unless overridden
        {"xlingsBinary", s(registry / "bin" /
                           ("xlings" + std::string(mcpp::platform::exe_suffix)))},
        {"config",       s(config)},
        {"buildCache",   s(mcpp::home::cache_root())},
        // The engine interface a build program imports, as a package directory
        // (mcpp#797): written by the first build program of this version, read
        // by an editor (`mcpp:core` in the build database).
        {"engineModules", s(mcpp::build::engine::package_dir())},
        {"mcppVersion",  std::string(mcpp::toolchain::MCPP_VERSION)},
        // What a build with nothing configured resolves on this host (WS8):
        // the one function the first run uses. Probing for a usable MSVC is
        // read-only, as the rest of this path is.
        {"defaultToolchain", std::string(mcpp::toolchain::triple::pins::host_default_toolchain(
            mcpp::toolchain::msvc::msvc_available_here(mcpp::provider::layout().store_of_home(registry))))},
        // An Android host (Termux): the same default toolchain as GNU/Linux on
        // this architecture; reported because the shell and the loader rules
        // differ (D21, D28).
        {"android",      mcpp::platform::linux_::is_android_host()},
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
    // `--format` IS ONLY DEFINED FOR THE `toolchain` TOPIC, and saying so
    // beats emitting an envelope whose `data` silently omits what was asked
    // for. `runtime` and `deps` have their own shapes and no consumer yet;
    // inventing one here would publish a contract nobody has read.
    if (auto f = parsed.value("format")) {
        auto fmt = mcpp::wire::parse_format(*f);
        if (!fmt) {
            std::println(stderr, "error: {}",
                         mcpp::wire::unsupported_format(*f));
            return 2;
        }
        const std::string topic = parsed.positional(0);
        if (topic == "sources" || topic == "tool" || topic == "payload")
            return mcpp::doctor::why_sources_json(topic, parsed.positional(1),
                                                  parsed.option_or_empty("features").value());
        if (topic == "build-programs" || topic == "compat")
            return mcpp::doctor::why_build_programs_or_compat_json(
                topic, parsed.option_or_empty("features").value());
        if (!topic.empty() && topic != "toolchain") {
            std::println(stderr,
                "error: --format json is defined for `mcpp why toolchain`, "
                "`mcpp why sources|tool|payload`, `mcpp why build-programs` and "
                "`mcpp why compat`; '{}' has no machine-readable "
                "shape yet", topic);
            return 2;
        }
        return mcpp::doctor::why_toolchain_json(
            parsed.option_or_empty("target").value(),
            parsed.option_or_empty("toolchain").value());
    }
    return mcpp::doctor::why_report(parsed.positional(0),
                                    parsed.option_or_empty("features").value(),
                                    parsed.positional(1));
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
    std::string target = parsed.option_or_empty("home").value();
    if (parsed.value("home") && target.empty()) {
        std::println(stderr, "error: --home requires a non-empty directory");
        return 2;
    }
    if constexpr (mcpp::platform::is_windows) {
        const auto origin = mcpp::home::original_root();
        std::error_code ec;
        const auto* explicitHome = std::getenv("MCPP_HOME");
        // Only the user-directory fallback participates. An installed portable
        // tree and an explicitly selected original home retain their behavior.
        const bool userFallback = mcpp::home::original_source() == "user";
        if (target.empty() && !(explicitHome && *explicitHome) && userFallback
            && !mcpp::home::initialized(origin)
            && !std::filesystem::exists(origin / "home.toml", ec) && !ec
            && mcpp::platform::terminal::input_is_terminal()
            && mcpp::platform::terminal::is_terminal(mcpp::platform::terminal::Stream::Err)
            && mcpp::platform::windows::writable_fixed_drive('D', L"mcpp")) {
            auto fallback = mcpp::home::spelling(origin);
            std::ranges::replace(fallback, '/', '\\');
            mcpp::platform::terminal::write(mcpp::platform::terminal::Stream::Err,
                std::format("        Home No existing home found\n"
                            "       Drive D:\\ is a writable local fixed drive\n\n"
                            "      Choose the mcpp home:\n\n"
                            "             1) D:\\mcpp                  [default]\n"
                            "             2) {}\n\n"
                            "      Select [1/2, Enter=1]: ", fallback));
            std::cerr.flush();
            std::string input;
            if (!std::getline(std::cin, input)) return 1;
            const auto first = input.find_first_not_of(" \t\r");
            input = first == std::string::npos ? ""
                  : input.substr(first, input.find_last_not_of(" \t\r") - first + 1);
            if (input.empty() || input == "1") target = "D:/mcpp";
            else if (input == "2") target = fallback;
            else {
                std::println(stderr, "       Error Invalid selection; expected 1 or 2\n"
                                     "        Hint For a custom path, run: mcpp self init --home <path>");
                return 2;
            }
        }
    }
    return mcpp::doctor::self_init(parsed.is_flag_set("force"), target);
}

export int cmd_self_config(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::doctor::self_config(parsed.option_or_empty("mirror").value());
}

// `mcpp self store dedupe [--apply]`.
export int cmd_self_store(const mcpplibs::cmdline::ParsedArgs& parsed) {
    auto sub = parsed.has_subcommand() && parsed.subcommand_name() == "dedupe" ? parsed.subcommand() : std::nullopt;
    if (!sub) {
        std::println(stderr, "error: `mcpp self store` requires a subcommand: dedupe");
        return 2;
    }
    return mcpp::doctor::store_dedupe(sub->get().is_flag_set("apply"));
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
