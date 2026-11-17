// mcpp.bmi — the cutoff by BMI content, for every compiler.
//
// A module edge keeps the BMI before it, compiles, and asks here whether the
// previous file stays: where it does, ninja's `restat` leaves the importers
// clean. This module is the part no compiler owns:
//
//   difference      two BMIs compared; a format's reader is asked where the
//                   bytes are a format mcpp reads, else the bytes decide
//   keeps_previous  the question an edge asks, under the build directory's
//                   cutoff, recording what it found
//   run_begin/end   the cutoff and the record of one run of a build directory
//   summary         what a run says of its BMIs
//
// What is specific to one compiler's file format is a module of its own
// beside this one: mcpp.bmi.gcc reads a `.gcm` (two stamps, line maps in a
// section of their own). Clang's `.pcm` has no reader: it holds no stamp and
// no structure that isolates locations (mcpp#946 O4), so its bytes decide.
// A reader for another format is added as a module and one line of
// `difference`.

export module mcpp.bmi;

import std;
export import mcpp.bmi.difference;
import mcpp.bmi.gcc;

export namespace mcpp::bmi {

// What the two files differ in, by the reader of their format.
Difference difference(const std::filesystem::path& a, const std::filesystem::path& b);

// THE CUTOFF OF ONE BUILD DIRECTORY (`[profile.<name>] bmi_cutoff`, SPEC-012).
//
// Three files of the build directory, which is the directory every edge of
// its graph runs in: `bmi-cutoff` states the cutoff (`ignore-locations`;
// absent or anything else: `exact`), written where the graph is;
// `bmi-changes.tsv` is what the edges of the current run found, one line per
// BMI that is not the same (`<bmi>\t<changed|locations>\t<kept|replaced>`);
// `bmi-kept.list` names the BMIs kept with the lines of an earlier source.
// A file rather than a word of each command: three schedules compare a BMI
// (the rule, the detached code generation, the compiler transaction), and
// the cutoff is one statement of the directory.
inline constexpr std::string_view kCutoffFile  = "bmi-cutoff";
inline constexpr std::string_view kChangesFile = "bmi-changes.tsv";
inline constexpr std::string_view kKeptFile    = "bmi-kept.list";
inline constexpr std::string_view kIgnoreLocations = "ignore-locations";

// What an edge asks after compiling an interface: whether the BMI before it
// stays. Reads the cutoff and records the difference in the current
// directory.
bool keeps_previous(const std::filesystem::path& bmi, const std::filesystem::path& previous);

// Before a run of the graph in `dir`: states the cutoff, starts the record of
// changes, and, where the cutoff is exact, removes every BMI an earlier run
// kept with earlier lines, so that its edge runs and its importers follow.
// The products are then what a build from nothing gives.
// `ignoreLocations` empty: the cutoff the directory already states (a run of
// a graph that is not written again).
void run_begin(const std::filesystem::path& dir, std::optional<bool> ignoreLocations);

// After the run: what its edges found, and the list of kept BMIs brought up
// to date. `changed` and `kept` are the names of the BMIs, without directory.
struct Run {
    std::vector<std::string> changed;   // replaced: their importers were compiled again
    std::vector<std::string> kept;      // locations only, kept: their importers were not
};
Run run_end(const std::filesystem::path& dir);

// What a run says of its BMIs after it, one line each; empty when none changed.
// `the BMI of c.gcm changed: its importers were compiled again`, and for a
// kept one what was not done and the key that states otherwise.
std::vector<std::string> summary(const Run& run);

} // namespace mcpp::bmi

namespace mcpp::bmi {

Difference difference(const std::filesystem::path& a, const std::filesystem::path& b) {
    char head[52] = {};
    std::ifstream in(a, std::ios::binary);
    in.read(head, sizeof head);
    if (gcc::is_format(std::string_view(head, static_cast<std::size_t>(in.gcount())))) return gcc::difference(a, b);
    std::error_code ea, eb;
    const auto sa = std::filesystem::file_size(a, ea), sb = std::filesystem::file_size(b, eb);
    if (ea || eb || sa != sb) return Difference::Changed;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    return fa && fb && std::equal(std::istreambuf_iterator<char>(fa), {}, std::istreambuf_iterator<char>(fb))
        ? Difference::Same : Difference::Changed;
}

namespace {
std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}
} // namespace

bool keeps_previous(const std::filesystem::path& bmi, const std::filesystem::path& previous) {
    const auto found = bmi::difference(bmi, previous);
    if (found == Difference::Same) return true;
    bool keep = false;
    if (found == Difference::LocationsOnly) {
        const auto cutoff = read_lines(std::filesystem::path{kCutoffFile});
        keep = !cutoff.empty() && cutoff.front() == kIgnoreLocations;
    }
    // One write of one line, appended: the edges of a run do not interleave.
    const auto line = std::format("{}\t{}\t{}\n", bmi.generic_string(),
        found == Difference::LocationsOnly ? "locations" : "changed", keep ? "kept" : "replaced");
    std::ofstream out(std::filesystem::path{kChangesFile}, std::ios::app | std::ios::binary);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    return keep;
}

void run_begin(const std::filesystem::path& dir, std::optional<bool> ignoreLocations) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    const auto cutoffFile = dir / kCutoffFile;
    const auto have = read_lines(cutoffFile);
    const bool ignore = ignoreLocations.value_or(!have.empty() && have.front() == kIgnoreLocations);
    const std::string_view want = ignore ? kIgnoreLocations : std::string_view("exact");
    if (have.empty() || have.front() != want)
        std::ofstream(cutoffFile, std::ios::trunc | std::ios::binary) << want << "\n";
    std::filesystem::remove(dir / kChangesFile, ec);
    if (ignore) return;
    const auto keptFile = dir / kKeptFile;
    for (auto const& bmi : read_lines(keptFile)) std::filesystem::remove(dir / bmi, ec);
    std::filesystem::remove(keptFile, ec);
}

std::vector<std::string> summary(const Run& run) {
    const auto names = [](const std::vector<std::string>& v) {
        std::string s;
        constexpr std::size_t kShown = 4;
        for (std::size_t i = 0; i < v.size() && i < kShown; ++i) s += (i ? ", " : "") + v[i];
        if (v.size() > kShown) s += std::format(" (+{})", v.size() - kShown);
        return s;
    };
    std::vector<std::string> lines;
    if (!run.changed.empty())
        lines.push_back(std::format("{} {} changed: {} importers were compiled again",
                                    run.changed.size() == 1 ? "the BMI" : "the BMIs", names(run.changed),
                                    run.changed.size() == 1 ? "its" : "their"));
    if (!run.kept.empty())
        lines.push_back(std::format("{} {} changed in locations only and {} kept: importers were not compiled "
                                    "again and name the earlier lines (bmi_cutoff = \"exact\" compiles them)",
                                    run.kept.size() == 1 ? "the BMI" : "the BMIs", names(run.kept),
                                    run.kept.size() == 1 ? "is" : "are"));
    return lines;
}

Run run_end(const std::filesystem::path& dir) {
    Run run;
    const auto keptFile = dir / kKeptFile;
    auto kept = read_lines(keptFile);
    const auto before = kept;
    for (auto const& line : read_lines(dir / kChangesFile)) {
        const auto a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || a == b) continue;
        const auto bmi = line.substr(0, a);
        const auto name = std::filesystem::path(bmi).filename().string();
        if (line.substr(b + 1) == "kept") {
            run.kept.push_back(name);
            if (!std::ranges::contains(kept, bmi)) kept.push_back(bmi);
        } else {
            run.changed.push_back(name);
            std::erase(kept, bmi);
        }
    }
    if (kept != before) {
        std::error_code ec;
        if (kept.empty()) std::filesystem::remove(keptFile, ec);
        else {
            std::ofstream out(keptFile, std::ios::trunc | std::ios::binary);
            for (auto const& bmi : kept) out << bmi << "\n";
        }
    }
    return run;
}

} // namespace mcpp::bmi
