// mcpp.cli.cmd_cache — CLI parsing + routing for the `mcpp cache` family.
// Implementations live in mcpp.bmi_cache.maintenance.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_cache;

import std;
import mcpplibs.cmdline;
import mcpp.bmi_cache.maintenance;
import mcpp.ui;
import mcpp.wire;
import mcpp.libs.json;

namespace mcpp::cli {

export int cmd_cache_dir(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::bmi_cache::cache_dir();
}

export int cmd_cache_list(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // `--format json` is enveloped; `--json` keeps the payload it shipped
    // with (`{root, entries}` at the top level, which this repo's own e2e
    // asserts). Spelling compatibility is not payload compatibility, and
    // wrapping the old spelling would break every consumer that already
    // reads it.
    if (auto f = parsed.value("format")) {
        auto fmt = mcpp::wire::parse_format(*f);
        if (!fmt) {
            std::println(stderr, "error: {}", mcpp::wire::unsupported_format(*f));
            return 2;
        }
        mcpp::wire::emit({
            .kind = "mcpp.cache",
            .data = mcpp::bmi_cache::cache_list_json(),
        });
        return 0;
    }
    return mcpp::bmi_cache::cache_list(parsed.is_flag_set("json"));
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

export int cmd_cache_gc(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::bmi_cache::cache_gc(
        parsed.option_or_empty("max-size").value(),
        parsed.option_or_empty("older-than").value());
}

export int cmd_cache_clean(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::bmi_cache::cache_clean(parsed.is_flag_set("deps"),
                                        parsed.is_flag_set("std"),
                                        parsed.is_flag_set("all"),
                                        parsed.is_flag_set("legacy"));
}

export int cmd_cache_verify(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::bmi_cache::cache_verify();
}

} // namespace mcpp::cli
