// mcpp.cli.cmd_registry — CLI parsing + routing for search and the
// `mcpp index` family. Implementations live in mcpp.pm.index_ops.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_registry;

import std;
import mcpplibs.cmdline;
import mcpp.pm.index_ops;
import mcpp.ui;

namespace mcpp::cli {

export int cmd_search(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string keyword = parsed.positional(0);
    if (keyword.empty()) {
        std::println(stderr, "error: `mcpp search` requires a keyword");
        return 2;
    }
    return mcpp::pm::search_packages(keyword);
}

export int cmd_index_list(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::pm::index_list();
}

export int cmd_index_add(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    std::string url  = parsed.positional(1);
    if (name.empty() || url.empty()) {
        mcpp::ui::error("usage: mcpp index add <name> <url>");
        return 2;
    }
    return mcpp::pm::index_add(name, url);
}

export int cmd_index_remove(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        mcpp::ui::error("usage: mcpp index remove <name>");
        return 2;
    }
    return mcpp::pm::index_remove(name);
}

export int cmd_index_update(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::pm::index_update(parsed.positional(0));
}

export int cmd_index_pin(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        mcpp::ui::error("usage: mcpp index pin <name> [<rev>]");
        return 2;
    }
    return mcpp::pm::index_pin(name, parsed.positional(1));
}

export int cmd_index_unpin(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        mcpp::ui::error("usage: mcpp index unpin <name>");
        return 2;
    }
    return mcpp::pm::index_unpin(name);
}

} // namespace mcpp::cli
