// mcpp.cli.cmd_toolchain — CLI parsing + routing for the `mcpp toolchain`
// family. Implementations live in mcpp.toolchain.lifecycle.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_toolchain;

import std;
import mcpplibs.cmdline;
import mcpp.config;
import mcpp.fetcher.progress;
import mcpp.toolchain.lifecycle;
import mcpp.ui;

namespace mcpp::cli {

export int cmd_toolchain(const mcpplibs::cmdline::ParsedArgs& parsed) {
    auto subname = parsed.subcommand_name();
    if (subname.empty()) {
        mcpp::ui::error("`mcpp toolchain` requires a subcommand: install / list / default / remove");
        return 2;
    }
    const mcpplibs::cmdline::ParsedArgs& sub_parsed = *parsed.subcommand_matches;

    // Surface bootstrap (xim:patchelf, xim:ninja) as a progress bar — same
    // treatment any other download here gets, so the user isn't staring at
    // a quiet "Bootstrap…" line during a slow link.
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    // The optional --target axis (install/default/remove): which target's
    // payload the command operates on. Omitted = host.
    std::string targetArg;
    if (auto t = sub_parsed.value("target")) targetArg = *t;

    if (subname == "list")
        return mcpp::toolchain::toolchain_list(*cfg);
    if (subname == "install")
        return mcpp::toolchain::toolchain_install(
            *cfg, sub_parsed.positional(0), sub_parsed.positional(1), targetArg);
    if (subname == "default")
        return mcpp::toolchain::toolchain_set_default(
            *cfg, sub_parsed.positional(0), sub_parsed.positional(1), targetArg);
    if (subname == "remove")
        return mcpp::toolchain::toolchain_remove(*cfg, sub_parsed.positional(0), targetArg);

    mcpp::ui::error(std::format("unknown toolchain subcommand '{}'", subname));
    return 2;
}

} // namespace mcpp::cli
