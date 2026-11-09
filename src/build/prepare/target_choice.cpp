// The selection context of a configuration (SPEC-016 §4): the host the
// candidates' availability is asked of, the probes, the declared family and the
// provider chosen by name, and the priorities the project and the machine set.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.prepare_inputs;
import mcpp.manifest;
import mcpp.platform;
import mcpp.toolchain.platform;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_select;

namespace mcpp::build {

namespace mp = mcpp::manifest::platform;

mcpp::toolchain::target::Context target_context(std::span<const mp::Override> project,
                                                std::span<const mp::Override> config,
                                                std::string family, std::string provider,
                                                std::function<bool()> msvcInstalled) {
    mcpp::toolchain::target::Context ctx;
    ctx.host = mcpp::toolchain::target::host_pair(mcpp::toolchain::platform::name,
                                                  mcpp::toolchain::platform::host_arch);
    ctx.probe = [msvc = std::move(msvcInstalled)](std::string_view p) {
        return p == "msvc" && msvc && msvc();
    };
    ctx.family = std::move(family);
    ctx.provider = std::move(provider);
    // The project's tables over the machine's; within one, the most specific
    // matching table that sets the provider's priority.
    std::vector<mp::Override> p(project.begin(), project.end()), c(config.begin(), config.end());
    ctx.priority = [p = std::move(p), c = std::move(c)](const mcpp::toolchain::target::Row& row,
                                                        std::string_view name) -> std::optional<int> {
        const auto cfg = cfgpred::context_for(row.triple);
        auto matches = [&](std::string_view sel) { return cfgpred::matches(std::string(sel), cfg); };
        for (auto const* tables : { &p, &c }) {
            std::optional<int> found;
            for (auto const* o : mcpp::platform::matching(*tables, matches))
                if (auto it = o->priority.find(std::string(name)); it != o->priority.end()) found = it->second;
            if (found) return found;
        }
        return std::nullopt;
    };
    return ctx;
}

} // namespace mcpp::build
