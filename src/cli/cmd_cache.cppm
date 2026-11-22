// mcpp.cli.cmd_cache — CLI parsing + routing for the `mcpp cache` family.
// Implementations live in mcpp.bmi_cache.maintenance.

export module mcpp.cli.cmd_cache;

import std;
import mcpplibs.mwrap.libc;
import mcpplibs.cmdline;
import mcpp.bmi_cache.maintenance;
import mcpp.config;
import mcpp.provider.roots;
import mcpp.ui;
import mcpp.wire;
import nlohmann.json;

namespace libc = mcpplibs::mwrap::libc::macros;

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
            std::println(libc::stderr(), "error: {}", mcpp::wire::unsupported_format(*f));
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

// THE CACHE, THEN THE STORE (design 2026-11-22 §3.11). The store's payloads
// are collected by the provider, whose one implementation keeps what anything
// references; mcpp's part is its roots: with `--older-than`, the roots of
// payloads no build used for that long are removed first.
export int cmd_cache_gc(const mcpplibs::cmdline::ParsedArgs& parsed) {
    const bool dryRun = parsed.is_flag_set("dry-run");
    const auto olderThan = parsed.option_or_empty("older-than").value();
    if (int rc = mcpp::bmi_cache::cache_gc(parsed.option_or_empty("max-size").value(), olderThan, dryRun); rc != 0)
        return rc;
    if (!olderThan.empty()) {
        const auto secs = mcpp::bmi_cache::parse_duration(olderThan).value_or(0);
        for (const auto& r : mcpp::provider::roots::expire(secs, dryRun))
            mcpp::ui::status(dryRun ? "Would release" : "Released",
                             std::format("the store's root {} (last used {})", r.name,
                                         mcpp::bmi_cache::age_text(r.lastUse)));
    }
    // Read only: a home whose provider was never set up has no store to
    // collect, and collecting is no reason to set one up.
    auto cfg = mcpp::config::load_or_init(/*quiet=*/true, {}, {}, mcpp::config::Initialization::ReadOnly);
    if (!cfg) {
        mcpp::ui::error(cfg.error().message);
        return 1;
    }
    // A home's roots before its first collection: without them, the provider
    // would remove every payload a build of this home uses.
    if (!mcpp::provider::roots::seed()) {
        mcpp::ui::error("the store is not collected: its roots could not be registered (see the warning above)");
        return 1;
    }
    return mcpp::config::collect_store(*cfg, dryRun);
}

export int cmd_cache_clean(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::bmi_cache::cache_clean(parsed.is_flag_set("deps"),
                                        parsed.is_flag_set("std"),
                                        parsed.is_flag_set("tools"),
                                        parsed.is_flag_set("all"),
                                        parsed.is_flag_set("legacy"));
}

export int cmd_cache_verify(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    return mcpp::bmi_cache::cache_verify();
}

} // namespace mcpp::cli
