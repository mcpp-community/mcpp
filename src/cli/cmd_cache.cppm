// mcpp.cli.cmd_cache — CLI parsing + routing for the `mcpp cache` family.
// Implementations live in mcpp.bmi_cache.ops.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_cache;

import std;
import mcpplibs.cmdline;
import mcpp.bmi_cache.ops;
import mcpp.ui;

namespace mcpp::cli {

export int cmd_cache_list(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::bmi_cache::cache_list();
}

export int cmd_cache_info(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string needle = parsed.positional(0);
    if (needle.empty()) {
        mcpp::ui::error("usage: mcpp cache info <pkg>@<ver>");
        return 2;
    }
    return mcpp::bmi_cache::cache_info(needle);
}

export int cmd_cache_prune(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::bmi_cache::cache_prune(parsed.option_or_empty("older-than").value());
}

export int cmd_cache_clean(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::bmi_cache::cache_clean();
}

} // namespace mcpp::cli
