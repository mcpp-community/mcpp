// build_dir.cpp -- a configuration's build directory over time (design
// 2026-10-31 §8, 2026-11-12 §17): named by its lineage, emptied when its
// compiler changes, taking over a directory an earlier mcpp named by the whole
// fingerprint, and what it records for the next build and for `mcpp clean --stale`.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.bmi_cache.maintenance;   // dir_size, human_bytes
import mcpp.build.dirlock;
import mcpp.build.graph_engine;
import mcpp.build.cache_key;         // kCacheEpoch: the epoch the graph's engine link carries
import mcpp.home.files;
import mcpp.log;
import mcpp.toolchain.fingerprint;
import mcpp.ui;

namespace mcpp::build {

namespace {

using mcpp::toolchain::lineage_text, mcpp::toolchain::toolchain_text;

// THE COMPILER RECORDS OF THE PREDECESSOR ARE NOT TAKEN OVER. Beside each object
// a split compile keeps its slot's records (`<object>.sched.*`, compiler
// transactions): the attempt's transaction, its completion certificate, its
// status and the BMI it preserved. They name the predecessor's paths, and a
// certificate that names another directory's object is refused at the next
// build's admission ("cached compiler certificate does not name its own
// object") -- every build of the configuration after an mcpp upgrade failed so.
// They describe attempts made there, and none is running (the predecessor's
// lock is held), so they are removed: an object they would have admitted is
// compiled again.
void drop_compiler_records(const std::filesystem::path& objects) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> records;
    for (fs::recursive_directory_iterator it(objects, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.ends_with(".sched") || name.find(".sched.") != std::string::npos) records.push_back(it->path());
    }
    for (const auto& r : records) {
        std::error_code rec;
        fs::remove_all(r, rec);
    }
}

std::string read_small(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

// A DIRECTORY AN EARLIER MCPP NAMED BY THE WHOLE FINGERPRINT (before
// 2026.11.13.1), or one this configuration left under another name, is taken
// over once by the lineage it records; from then on the name holds.
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
        drop_compiler_records(outputDir / "obj");
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

std::filesystem::path renew_build_dir(const PrepareState& state, const std::filesystem::path& outputDir) {
    namespace fs = std::filesystem;
    const auto was = read_small(outputDir / "toolchain");
    if (!was.empty() && was != toolchain_text(state.fp)) {
        // Held by this build (dirlock): nothing else writes here.
        const auto bytes = mcpp::bmi_cache::dir_size(outputDir);
        const auto lock = std::string(mcpp::build::dirlock::kLockName);
        std::error_code ec;
        for (auto const& e : fs::directory_iterator(outputDir, ec)) {
            const auto name = e.path().filename().string();
            if (name == lock || name == lock + ".owner") continue;
            std::error_code rec;
            fs::remove_all(e.path(), rec);
        }
        mcpp::ui::status("Replaced", std::format(
            "{} ({}): the toolchain changed, and nothing compiled there is reused",
            outputDir.lexically_relative(state.workRoot).generic_string(), mcpp::bmi_cache::human_bytes(bytes)));
    } else if (const auto fp = read_small(outputDir / "fingerprint"); !fp.empty() && fp != state.fp.hex + "\n") {
        // Another mcpp, std module or runtime contract continues here: the
        // compiler records describe the attempts of the build before, as those
        // of a directory taken over do.
        drop_compiler_records(outputDir / "obj");
    }
    return mcpp::build::graph_engine::refresh(outputDir, mcpp::build::cache_key::kCacheEpoch);
}

// What this configuration uses beside its directory (`mcpp clean --stale`); its
// lineage and toolchain, which the next build of the configuration reads; and
// the whole fingerprint, which the directory's name no longer states.
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
                                      std::pair{"toolchain", toolchain_text(state.fp)},
                                      std::pair{"fingerprint", state.fp.hex + "\n"}})
        if (read_small(outputDir / name) != text)
            std::ofstream(outputDir / name, std::ios::binary) << text;
}

} // namespace mcpp::build
