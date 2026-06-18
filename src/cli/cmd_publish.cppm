// mcpp.cli.cmd_publish — CLI parsing + routing for publish / pack /
// emit xpkg. Implementations live in mcpp.publish.pipeline and mcpp.pack.pipeline.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_publish;

import std;
import mcpplibs.cmdline;
import mcpp.pack;
import mcpp.pack.pipeline;
import mcpp.publish.pipeline;
import mcpp.ui;

namespace mcpp::cli {

// `mcpp emit xpkg ...` — only one subcommand defined, so the action sits
// directly on the `emit xpkg` nested subcommand and receives its ParsedArgs.
export int cmd_emit_xpkg(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::publish::emit_xpkg_to(
        parsed.option_or_empty("version").value(),
        std::filesystem::path{parsed.option_or_empty("output").value()});
}

export int cmd_publish(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::publish::publish_package(
        parsed.is_flag_set("dry-run"), parsed.is_flag_set("allow-dirty"));
}

export int cmd_pack(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // ─── Resolve mode ────────────────────────────────────────────────
    mcpp::pack::Options opts;
    bool modeFromUser = false;
    if (auto v = parsed.value("mode")) {
        auto m = mcpp::pack::parse_mode(*v);
        if (!m) {
            mcpp::ui::error(std::format(
                "invalid --mode '{}'; expected: system | vendored | self-contained | static "
                "(aliases: bundle-project=vendored, bundle-all=self-contained)", *v));
            return 2;
        }
        opts.mode = *m;
        modeFromUser = true;
    }
    if (auto v = parsed.value("format")) {
        if (*v == "tar")      opts.format = mcpp::pack::Format::Tar;
        else if (*v == "dir") opts.format = mcpp::pack::Format::Dir;
        else {
            mcpp::ui::error(std::format(
                "invalid --format '{}'; expected tar | dir", *v));
            return 2;
        }
    }
    if (auto v = parsed.value("output")) opts.output = *v;
    if (auto v = parsed.value("target")) opts.targetTriple = *v;

    return mcpp::pack::build_and_pack(std::move(opts), modeFromUser);
}

} // namespace mcpp::cli
