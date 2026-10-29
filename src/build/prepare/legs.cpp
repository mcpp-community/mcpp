// legs.cpp -- the legs of one command, planned and built (SPEC-015 §3).
// Declared in the primary interface (prepare.cppm), where the comment that
// states the model is.

module mcpp.build.prepare;

import std;
import mcpp.build.progress;
import mcpp.build.refusal;
import mcpp.manifest;
import mcpp.toolchain.triple;
import mcpp.ui;

namespace mcpp::build::legs {

std::expected<Plan, std::string> plan(std::vector<Spec> specs, bool printFingerprint) {
    Plan out;
    for (auto& s : specs) {
        const std::string triple = s.overrides.target_triple;
        (void)refusal::take();
        auto ctx = prepare_build(printFingerprint, s.includeDevDeps, std::move(s.extraTargets),
                                 std::move(s.overrides));
        if (!ctx) {
            if (!s.fromSet.empty() && refusal::take() == refusal::Code::PlatformTargetUnserved) {
                out.leftOut.push_back(std::format("{}: {}", triple, ctx.error()));
                continue;
            }
            if (triple.empty()) return std::unexpected(ctx.error());
            return std::unexpected(std::format("the {} leg: {}", triple, ctx.error()));
        }
        out.legs.push_back({triple, s.fromSet, std::move(*ctx)});
    }
    for (auto const& why : out.leftOut)
        mcpp::ui::note(std::format("left out of the target set, its platform does not serve it -- {}",
                                   why));
    if (out.legs.empty() && !out.leftOut.empty())
        return std::unexpected("no member of the target set is served by its platform");
    return out;
}

std::vector<int> run_by_directory(std::span<const std::filesystem::path> directories,
                                  const std::function<int(std::size_t)>& work) {
    std::vector<int> results(directories.size(), 0);
    std::map<std::filesystem::path, std::vector<std::size_t>> byDirectory;
    for (std::size_t i = 0; i < directories.size(); ++i)
        byDirectory[directories[i].lexically_normal()].push_back(i);
    std::vector<std::jthread> runs;
    for (auto const& [dir, indices] : byDirectory)
        runs.emplace_back([&, indices] {
            for (auto i : indices) results[i] = work(i);
        });
    runs.clear();   // joins
    return results;
}

void share_jobs(BuildContext& ctx, std::size_t n) {
    const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t want = ctx.plan.scheduleNinjaJobs > 0
        ? static_cast<std::size_t>(ctx.plan.scheduleNinjaJobs) : hw + 2;
    ctx.plan.scheduleNinjaJobs = static_cast<int>(std::max<std::size_t>(1, want / std::max<std::size_t>(1, n)));
}

std::vector<int> build(std::span<BuildContext* const> contexts,
                       const std::function<int(BuildContext&)>& buildOne) {
    if (contexts.empty()) return {};
    if (contexts.size() == 1) return {buildOne(*contexts.front())};
    // One report for every leg: each package line names its configuration,
    // and one `Finished` follows them all.
    const bool reporting = mcpp::build::progress::is_open();
    if (reporting) {
        mcpp::build::progress::configurations(contexts.size());
        mcpp::build::progress::defer_finished();
    }
    std::vector<std::filesystem::path> dirs;
    for (auto* c : contexts) dirs.push_back(c->outputDir);
    const std::size_t distinct = std::set<std::filesystem::path>(
        dirs.begin(), dirs.end()).size();
    // The root compile database is the command's, published by the caller
    // from every leg's, not by each concurrent build.
    for (auto* c : contexts) {
        c->plan.publishRootCompileDb = false;
        share_jobs(*c, distinct);
    }
    auto results = run_by_directory(dirs, [&](std::size_t i) { return buildOne(*contexts[i]); });
    if (reporting && first_failure(results) == 0) mcpp::build::progress::finish_deferred();
    return results;
}

int first_failure(std::span<const int> statuses) {
    for (int r : statuses)
        if (r != 0) return r;
    return 0;
}

std::vector<std::string> native_first(std::span<const std::string> triples) {
    std::vector<std::string> native, other;
    for (auto const& t : triples) {
        auto p = mcpp::toolchain::triple::parse(t);
        (p && p->arch == mcpp::toolchain::triple::host_triple().arch ? native : other).push_back(t);
    }
    native.insert(native.end(), other.begin(), other.end());
    return native;
}

} // namespace mcpp::build::legs
