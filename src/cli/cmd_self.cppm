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

namespace mcpp::cli {

export int cmd_env(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
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
