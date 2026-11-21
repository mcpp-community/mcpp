import std;
import mcpplibs.mtest;
import mcpp.effects;
import mcpp.invocation;

using namespace mcpplibs::mtest;

// CROSS-LAYER: the keys that hold for a whole run, stated once as their variables by tier
// (SPEC-004 §2, `mcpp::effects::state_key`), and the sources an offline run installs from
// (design 2026-11-21, offline and toolchain defaults §3.2). The e2e suite states what a build
// does with them (tests/e2e/index/bundle.cpp).

namespace fx = mcpp::effects;

namespace {
// Each case starts with the variables unset and leaves them so.
struct Clean {
    mcpp::invocation::ScopedEnv offline{"MCPP_OFFLINE", std::nullopt};
    mcpp::invocation::ScopedEnv archives{"MCPP_ARCHIVES", std::nullopt};
    mcpp::invocation::ScopedEnv deny{"MCPP_DENY_EFFECTS", std::nullopt};
};
}  // namespace

UTest a_higher_tier_states_a_key_and_a_lower_one_does_not{"RunKeys/TierOrder", [] {
    Clean clean;
    fx::state_key("MCPP_OFFLINE", fx::Tier::Machine, "1", "config.toml [build] offline");
    expect(eq(mcpp::invocation::get("MCPP_OFFLINE").value_or(""), std::string("1")));
    expect(fx::key_origin("MCPP_OFFLINE").tier == fx::Tier::Machine);
    fx::state_key("MCPP_OFFLINE", fx::Tier::Project, "0", "mcpp.toml [build] offline");
    expect(eq(mcpp::invocation::get("MCPP_OFFLINE").value_or(""), std::string("0"))) << "the project's statement is below the machine's";
    expect(eq(fx::key_origin("MCPP_OFFLINE").where, std::string("mcpp.toml [build] offline")));
    fx::state_key("MCPP_OFFLINE", fx::Tier::Machine, "1", "config.toml [build] offline");
    expect(eq(mcpp::invocation::get("MCPP_OFFLINE").value_or(""), std::string("0"))) << "the machine's statement replaced the project's";
    expect(!fx::offline_mode());
}};

UTest a_variable_the_caller_set_is_the_override{"RunKeys/CallerOverrides", [] {
    Clean clean;
    // A value no tier writes: a statement holds while the variable keeps the value it wrote.
    mcpp::invocation::set("MCPP_OFFLINE", "yes");
    fx::state_key("MCPP_OFFLINE", fx::Tier::Project, "0", "mcpp.toml [build] offline");
    expect(eq(mcpp::invocation::get("MCPP_OFFLINE").value_or(""), std::string("yes"))) << "a tier replaced the caller's variable";
    expect(fx::key_origin("MCPP_OFFLINE").tier == fx::Tier::Override);
    expect(fx::offline_remedy().contains("--offline")) << fx::offline_remedy();
}};

UTest the_way_back_names_the_tier_that_made_the_run_offline{"RunKeys/OfflineRemedy", [] {
    Clean clean;
    fx::state_key("MCPP_OFFLINE", fx::Tier::Project, "1", "mcpp.toml [build] offline");
    expect(fx::offline_remedy().contains("mcpp.toml [build]")) << fx::offline_remedy();
}};

UTest offline_installs_only_from_named_archive_directories{"RunKeys/InstallSources", [] {
    Clean clean;
    expect(fx::install_sources() == fx::InstallSources::Network);
    mcpp::invocation::set("MCPP_OFFLINE", "yes");
    expect(fx::install_sources() == fx::InstallSources::None) << "offline without a directory asked a source";
    mcpp::invocation::set("MCPP_ARCHIVES", "");
    expect(fx::install_sources() == fx::InstallSources::None) << "an empty list named a directory";
    mcpp::invocation::set("MCPP_ARCHIVES", "/srv/a");
    expect(fx::install_sources() == fx::InstallSources::Local);
    expect(eq(fx::archive_dirs().size(), std::size_t{1}));
}};
