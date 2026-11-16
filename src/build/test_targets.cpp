// mcpp.build.test_targets: the predicates of `[target.<sel>.test]`, read as
// prepare reads every conditional section (mcpp#942). An implementation unit,
// so the evaluator stays out of the interface its importers read.
module mcpp.build.test_targets;

import std;
import mcpp.manifest;
import mcpp.build.prepare_inputs;

namespace mcpp::build {

std::expected<std::vector<std::string>, std::string>
conditional_test_globs(const mcpp::manifest::Manifest& m, std::string_view targetTriple) {
    std::vector<std::string> out;
    const auto ctx = cfgpred::context_for(targetTriple.empty() ? std::string_view(m.buildConfig.target)
                                                               : targetTriple);
    for (auto const& cc : m.conditionalConfigs) {
        if (cc.testDiscover.empty()) continue;
        // The tests are discovered before the graph is resolved, so a
        // target-side layer (`kernel-abi`, `c-abi`, ...) has no value yet; read
        // as false it would drop the tests without a word.
        if (cfgpred::uses_layer(cc.predicate))
            return std::unexpected(std::format(
                "[target.'{}'.test] names a target-side layer, which is resolved after the tests are "
                "discovered; select the tests by the target (os, arch, env, family, object-format)",
                cc.predicate));
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        out.insert(out.end(), cc.testDiscover.begin(), cc.testDiscover.end());
    }
    return out;
}

std::expected<std::vector<mcpp::manifest::DeployEntry>, std::string>
test_run_files(const mcpp::manifest::Manifest& m, std::string_view targetTriple) {
    std::vector<mcpp::manifest::DeployEntry> out = m.testFiles;
    const auto ctx = cfgpred::context_for(targetTriple.empty() ? std::string_view(m.buildConfig.target)
                                                               : targetTriple);
    for (auto const& cc : m.conditionalConfigs) {
        if (cc.testFiles.empty()) continue;
        if (cfgpred::uses_layer(cc.predicate))
            return std::unexpected(std::format(
                "[target.'{}'.test] files names a target-side layer; select the files by the target "
                "(os, arch, env, family, object-format)", cc.predicate));
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        out.insert(out.end(), cc.testFiles.begin(), cc.testFiles.end());
    }
    return out;
}

} // namespace mcpp::build
