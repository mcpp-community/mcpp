// build_dir.cpp -- a configuration's build directory over time (design
// 2026-10-31 §8): the predecessor a new fingerprint of its lineage takes over,
// and what the directory records for the next build and for `mcpp clean --stale`.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.bmi_cache.maintenance;   // dir_size, human_bytes
import mcpp.build.dirlock;
import mcpp.home.files;
import mcpp.log;
import mcpp.toolchain.fingerprint;
import mcpp.ui;

namespace mcpp::build {

namespace {

// A CONFIGURATION'S LINEAGE (design 2026-10-31 §8.2): the fingerprint's
// parts a developer switches between and keeps side by side -- the compiler
// family, the target, the standard library's family, the language standard and
// the configuration's flags. The rest evolve with time: the compiler's version
// and identity, the standard library's version, mcpp's version, the std
// module and the runtime contract. A new fingerprint of a lineage that has a
// directory takes that directory over instead of opening another copy of the
// build: ninja then rebuilds the edges whose commands changed and reuses the
// rest. Each mcpp upgrade used to open a full new directory (mcppls: 2.9 GB per
// configuration) and rebuild everything.
std::string lineage_text(const mcpp::toolchain::Fingerprint& fp) {
    const auto stdlibFamily = fp.parts[4].substr(0, fp.parts[4].find(' '));
    return std::format("compiler {}\ntarget {}\nstdlib {}\nstandard {}\nconfiguration {}\n",
                       fp.parts[0], fp.parts[3], stdlibFamily, fp.parts[5], fp.parts[6]);
}

// The toolchain's part of the evolution. Objects and BMIs of another compiler
// build are never reused, so a predecessor that differs here is removed rather
// than taken over: the build starts over in one directory instead of two.
std::string toolchain_text(const mcpp::toolchain::Fingerprint& fp) {
    return std::format("compiler {}\ndriver {}\nstdlib {}\n", fp.parts[1], fp.parts[2], fp.parts[4]);
}

std::string read_small(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

void adopt_predecessor_dir(const PrepareState& state, const std::filesystem::path& outputDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // A directory without a graph is not yet a build: the fast path takes the
    // lock of the directory the record names before it finds no graph there.
    if (fs::exists(outputDir / "build.ninja", ec)) return;
    const auto lineage = lineage_text(state.fp);
    std::optional<fs::path> best;
    fs::file_time_type bestTime{};
    for (auto const& d : fs::directory_iterator(outputDir.parent_path(), ec)) {
        std::error_code dec;
        if (d.path() == outputDir || !d.is_directory(dec)
            || read_small(d.path() / "lineage") != lineage)
            continue;
        // The one this configuration used last: build.ninja is written by every plan.
        const auto t = fs::last_write_time(d.path() / "build.ninja", dec);
        if (!dec && (!best || t > bestTime)) { best = d.path(); bestTime = t; }
    }
    if (!best) return;
    // A predecessor another build holds is left to it; this build opens its own.
    auto held = mcpp::home::FileLock::try_acquire(*best, mcpp::build::dirlock::kLockName);
    if (!held) return;
    const auto shown = best->lexically_relative(state.workRoot).generic_string();
    const bool sameToolchain = read_small(*best / "toolchain") == toolchain_text(state.fp);
    if (sameToolchain) {
        // Its entries move in, one rename each, beside the lock this build
        // may already have created there.
        fs::create_directories(outputDir, ec);
        const auto lock = std::string(mcpp::build::dirlock::kLockName);
        for (auto const& e : fs::directory_iterator(*best, ec)) {
            const auto name = e.path().filename().string();
            if (name == lock || name == lock + ".owner") continue;
            std::error_code mec;
            fs::remove_all(outputDir / name, mec);
            fs::rename(e.path(), outputDir / name, mec);
        }
        mcpp::log::info("build/dir", std::format("{} continues as {}", shown,
                        outputDir.lexically_relative(state.workRoot).generic_string()));
    }
    held.reset();
    const auto bytes = sameToolchain ? 0 : mcpp::bmi_cache::dir_size(*best);
    fs::remove_all(*best, ec);
    if (!sameToolchain && !ec)
        mcpp::ui::status("Replaced", std::format(
            "{} ({}): the toolchain changed, and nothing compiled there is reused", shown,
            mcpp::bmi_cache::human_bytes(bytes)));
}

// What this configuration uses beside its directory (`mcpp clean --stale`), and
// its lineage and toolchain, which the next fingerprint of the configuration
// reads to take the directory over.
void record_build_dir(const PrepareState& state, const std::filesystem::path& outputDir) {
    std::string uses;
    for (auto const& h : state.usedProgramOut) uses += "out " + h + "\n";
    for (auto const& g : state.usedHostGroups) uses += "host " + g + "\n";
    // The build-cache entries it used: the sweep after a build marks them used
    // for a build that replays this graph without planning.
    auto records = state.usedCacheRecords;
    if (state.describedStdModule)
        records.insert((state.describedStdModule->cacheDir / "std-module.json").string());
    for (auto const& r : records) uses += "cache " + r + "\n";
    std::error_code uec;
    std::filesystem::create_directories(outputDir, uec);
    std::ifstream in(outputDir / "uses.list", std::ios::binary);
    const std::string was{std::istreambuf_iterator<char>(in), {}};
    in.close();
    // Written when it is absent too: an empty list states that nothing is used.
    if (was != uses || !std::filesystem::exists(outputDir / "uses.list", uec))
        std::ofstream(outputDir / "uses.list", std::ios::binary) << uses;
    for (auto const& [name, text] : {std::pair{"lineage", lineage_text(state.fp)},
                                      std::pair{"toolchain", toolchain_text(state.fp)}})
        if (read_small(outputDir / name) != text)
            std::ofstream(outputDir / name, std::ios::binary) << text;
}

} // namespace mcpp::build
