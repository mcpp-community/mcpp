// mcpp.build.progress — what a build reports while it runs, and how
// (.agents/docs/2026-09-29-build-progress-display-design.md, and its revision
// 3, .agents/docs/2026-09-30-build-output-refinement-design.md).
//
// Four sources feed one model, and mcpp.ui draws it:
//
//   - mcpp's own steps (build programs, the phases), reported by the code that
//     performs them;
//   - ninja's status lines, one per finished step (`NINJA_STATUS`), which give
//     the counts and pace the reading;
//   - ninja's log, which states which step finished, when it started and when
//     it ended;
//   - the start file, where the engine's action wrapper writes the start of a
//     `check` or `prepare` action, the only steps whose start is observable.
//
// The step record, written beside build.ninja, names the package each step
// belongs to. A PACKAGE IS NAMED WHEN IT DOES WORK (revision 3, §5.3): its line
// is written when the first of its steps finishes, or when its first check or
// prepare action starts, and it never changes. Revision 2 wrote a package's
// line when every step the record assigns to it had finished; the steps of a
// package the cache serves run in a pass of their own and never counted, so
// such a package never completed and the folded dependency line waited for
// ninja to exit (measured: 27 s late in a first build of xlings).
//
// LOCKS. The model has one mutex. mcpp.ui asks the model for its frame while
// holding its own lock, so the model never calls mcpp.ui while holding the
// model's: every line the model writes is collected under the lock and written
// after it is released.

module;
#include <cstdio>

export module mcpp.build.progress;

import std;
import mcpp.ui;
import mcpp.ui.dots_screen;
import mcpp.log;
import mcpp.platform;

export namespace mcpp::build::progress {

// ─── The step record (design §6.3) ───────────────────────────────────────

struct PackageInfo {
    std::string name;              // qualified name: what the emitter records
    bool        requested   = false;
    std::string subject;           // the name as the package's line shows it
    std::size_t cachedUnits = 0;   // units staged from the global cache
    std::size_t steps       = 0;   // steps the graph assigns to it
    std::string detail;            // its version and origin, shown dim
    std::string source;            // project, official, index, git or path
};

struct Record {
    std::vector<PackageInfo> packages;
    // An output path, normalised, to its step's package in `packages`.
    std::unordered_map<std::string, std::size_t> owner;
    // An output path, normalised, to its step: the outputs of one step share
    // it, so a step whose log entries are read in two pieces counts once.
    std::unordered_map<std::string, std::size_t> step;
    // The first output of a check or prepare action, normalised, to its label.
    std::unordered_map<std::string, std::string> actions;
    std::size_t steps = 0;         // every step of the graph
};

inline constexpr std::string_view kRecordFile = "steps.tsv";

// A path as the record and the readers compare it: forward slashes, no `.`
// segments. ninja's log writes paths as build.ninja spelled them; a stamp the
// action wrapper reports was spelled by the same plan.
std::string normalise(std::string_view path);

std::string format_record(const Record& record);
Record parse_record(std::string_view text);
void write_record(const std::filesystem::path& buildDir, const Record& record);
std::optional<Record> read_record(const std::filesystem::path& buildDir);

// What the emitter states about each step as it writes it: the package whose
// unit, link, action or staged file the statement is for. `owner("")` marks
// the statements that follow as the build's own.
class Attribution {
public:
    struct Step {
        std::vector<std::string> outputs;   // normalised
        std::string              rule;
        std::string              owner;
    };
    void owner(std::string_view package);
    // Text appended to build.ninja: each `build` statement in it is recorded
    // with the current owner. Phony statements are not steps.
    void statement(std::string_view text);
    // A check or prepare action's first output and label.
    void action(std::string_view firstOutput, std::string_view label);
    const std::vector<Step>& steps() const { return steps_; }
    // The record: `declared` states how the packages are shown; an owner that
    // no entry declares is shown by its qualified name, as a dependency.
    Record record(const std::vector<PackageInfo>& declared) const;

private:
    std::string owner_;
    std::vector<Step> steps_;
    std::vector<std::pair<std::string, std::string>> actions_;
};

// ─── The readers (design §6) ─────────────────────────────────────────────

// Set as NINJA_STATUS: ninja prints it before the description of each step
// it finishes, with the steps finished, the steps planned and the step's end
// time in seconds since ninja started.
//
// IT BEGINS WITH AN ESCAPE SEQUENCE ON PURPOSE. A step's command inherits
// ninja's environment, so a ninja it runs (a `prepare` action's CMake or
// vcpkg build) prints the same marker, and the outer ninja relays that output
// after the step. ninja prints its own status line as it is, and strips
// escape sequences from a command's output when its standard output is not a
// terminal, as it is not here: only the outer ninja's lines keep the leading
// `ESC [ 0 m`. The outer ninja runs with CLICOLOR_FORCE=0, which keeps that
// stripping on.
//
// `%u` is the number of steps not yet started. Measured with ninja 1.12.1
// through a pipe, it reaches 0 when the last step starts, and from then on
// every one of the `t - f` remaining steps is running: the status row can say
// `last N running` at the end of a build and be exact.
inline constexpr std::string_view kStatusFormat = "\x1b[0m@@mcpp %f %t %e %u@@ ";

struct StatusLine {
    std::size_t      finished = 0;
    std::size_t      total    = 0;
    long long        endMs    = 0;
    std::optional<std::size_t> unstarted;   // absent in a line without `%u`
    std::string_view text;     // the description, or the command under -v
};
std::optional<StatusLine> parse_status(std::string_view line);

// One step of ninja's log: its entries share start, end and command hash.
struct LogStep {
    long long                start = 0;   // ms since that ninja started
    long long                end   = 0;
    std::vector<std::string> outputs;     // normalised
};
// The complete entries of `text`, which starts at the beginning of a line,
// grouped by step. `consumed` is the length of the complete lines read.
// Header and comment lines are skipped; `*version` receives the format a
// header states.
std::vector<LogStep> parse_log(std::string_view text, std::size_t* consumed,
                               int* version = nullptr,
                               std::vector<std::size_t>* offsets = nullptr);
// The index of the first step of this run in `steps`: the suffix whose end
// times are among `ends`, the end times of the status lines seen (§6.2).
std::size_t run_boundary(const std::vector<LogStep>& steps, std::vector<long long> ends);

// The start file of `mcpp __action` (design §6.4).
inline constexpr std::string_view kStartsEnv  = "MCPP_ACTION_STARTS";
inline constexpr std::string_view kStartsFile = ".mcpp-action-starts";
struct ActionStart {
    std::string stamp;      // normalised
    long long   unixMs = 0;
};
std::vector<ActionStart> parse_starts(std::string_view text, std::size_t* consumed);
// Called by the wrapper before it runs the command: appends the start of the
// action whose first stamp is `stamp` to the file MCPP_ACTION_STARTS names,
// in one write. Nothing when the variable is unset.
void record_action_start(std::string_view stamp);

// ─── The model (design §4) ───────────────────────────────────────────────

enum class ProgramOutcome { Ran, Cached, Failed };

// Opens the report of this command: the region, its frame and its poll.
// `verbose` also names the packages with nothing to do and each package's
// steps and span at the end. The status row's display is chosen here
// (MCPP_PROGRESS: `random`, the default; an animation's name; `bar`; `plain`;
// `off`). Idempotent.
void open(bool verbose);
// Closes it: the region is erased. Idempotent.
void close();
// The number of configurations the command builds: with more than one, a
// package line names its configuration.
void configurations(std::size_t n);

// Build programs (design §4.2). Every program that runs or fails has a line;
// a program whose result is reused has one under --verbose.
void program_scheduled(std::string_view package, bool requested);
void program_compiling(std::string_view package, bool requested);
void program_running(std::string_view package, bool requested);
void program_finished(std::string_view package, bool requested, ProgramOutcome outcome,
                      std::chrono::milliseconds compile, std::chrono::milliseconds run);
void programs_done();

// The validations after ninja.
void checking();

// `Finished` (design §4.5): the whole command's time, and how it was spent.
void finished(std::string_view profile, std::string_view descriptor);
// A command that builds several configurations writes one `Finished`, after
// all of them: `finished` then only records what it was given, and
// `finish_deferred` writes it.
void defer_finished();
void finish_deferred();

// One build directory's ninja runs within this command.
class Build {
public:
    // Opaque: its definition is this module's own.
    struct Impl;

    explicit Build(const std::filesystem::path& dir);
    ~Build();
    Build(const Build&) = delete;
    Build& operator=(const Build&) = delete;

    // How the plan names its packages (the full path; the fast path reads
    // the record instead).
    void declare(std::vector<PackageInfo> packages);
    const std::vector<PackageInfo>& declared() const;
    void set_record(Record record);
    bool has_record() const;

    // The environment ninja runs with: NINJA_STATUS and the start file.
    std::vector<std::pair<std::string, std::string>> environment() const;
    // One ninja invocation.
    void pass_begin();
    void status(const StatusLine& line);
    // A `FAILED: <outputs>` line. For the command's first failure, returns
    // the failed step's package as a line names it (empty when the step is
    // the build's own), after which the caller writes
    // `error: build failed in <package>` and the step's diagnostics; nothing
    // for a later failure.
    std::optional<std::string> failed(std::string_view outputs);
    void pass_end();
    // The build ended. Under --verbose, the packages with nothing to do are
    // named, and each package that did work states its steps and span.
    void finish(bool success);

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace mcpp::build::progress

namespace mcpp::build::progress {

using Clock = std::chrono::steady_clock;
using ms    = std::chrono::milliseconds;

// ─── The step record ─────────────────────────────────────────────────────

std::string normalise(std::string_view path) {
    std::string s(path);
    std::ranges::replace(s, '\\', '/');
    return std::filesystem::path(s).lexically_normal().generic_string();
}

namespace {

// Tabs and line ends cannot appear in a field.
std::string field(std::string_view s) {
    std::string out(s);
    std::ranges::replace(out, '\t', ' ');
    std::ranges::replace(out, '\n', ' ');
    std::ranges::replace(out, '\r', ' ');
    return out;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> out;
    while (true) {
        auto t = line.find('\t');
        out.push_back(line.substr(0, t));
        if (t == std::string_view::npos) break;
        line.remove_prefix(t + 1);
    }
    return out;
}

std::size_t to_size(std::string_view s) {
    std::size_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

long long to_ll(std::string_view s) {
    long long v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

} // namespace

// Format: a header, then `P` lines (packages, in order), `A` lines (actions)
// and `O` lines (an output and its package's index). Version 2 adds a
// package's detail and source to its `P` line; a version 1 record (mcpp
// 2026.9.29.5) is read with both empty, and its subject then carries what
// version 1 wrote there.
std::string format_record(const Record& r) {
    std::string out = std::format("# mcpp steps v2\t{}\n", r.steps);
    for (auto const& p : r.packages)
        out += std::format("P\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", field(p.name),
                           p.requested ? 1 : 0, p.cachedUnits, p.steps, field(p.subject),
                           field(p.detail), field(p.source));
    std::vector<std::pair<std::string, std::string>> actions(r.actions.begin(), r.actions.end());
    std::ranges::sort(actions);
    for (auto const& [out1, label] : actions)
        out += std::format("A\t{}\t{}\n", field(out1), field(label));
    std::vector<std::pair<std::string, std::size_t>> owners(r.owner.begin(), r.owner.end());
    std::ranges::sort(owners);
    for (auto const& [path, index] : owners) {
        auto st = r.step.find(path);
        out += std::format("O\t{}\t{}\t{}\n", index,
                           st == r.step.end() ? std::size_t{0} : st->second, field(path));
    }
    return out;
}

Record parse_record(std::string_view text) {
    Record r;
    while (!text.empty()) {
        auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        auto f = split_tabs(line);
        if ((f[0] == "# mcpp steps v1" || f[0] == "# mcpp steps v2") && f.size() >= 2) {
            r.steps = to_size(f[1]);
            continue;
        }
        if (f[0] == "P" && f.size() >= 6) {
            r.packages.push_back({std::string(f[1]), f[2] == "1", std::string(f[5]),
                                  to_size(f[3]), to_size(f[4]),
                                  f.size() >= 8 ? std::string(f[6]) : std::string{},
                                  f.size() >= 8 ? std::string(f[7]) : std::string{}});
        } else if (f[0] == "A" && f.size() >= 3) {
            r.actions.emplace(std::string(f[1]), std::string(f[2]));
        } else if (f[0] == "O" && f.size() >= 4) {
            const auto index = to_size(f[1]);
            if (index < r.packages.size()) {
                r.owner.emplace(std::string(f[3]), index);
                r.step.emplace(std::string(f[3]), to_size(f[2]));
            }
        }
    }
    return r;
}

void write_record(const std::filesystem::path& buildDir, const Record& record) {
    std::ofstream f(buildDir / kRecordFile, std::ios::binary | std::ios::trunc);
    f << format_record(record);
}

std::optional<Record> read_record(const std::filesystem::path& buildDir) {
    std::ifstream f(buildDir / kRecordFile, std::ios::binary);
    if (!f) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(f), {}};
    return parse_record(text);
}

// ─── Attribution ─────────────────────────────────────────────────────────

void Attribution::owner(std::string_view package) { owner_ = std::string(package); }

namespace {

// The outputs and the rule of a `build` line: tokens up to the first colon
// ninja does not read as escaped, `|` dropped, `$ ` `$:` `$$` unescaped.
std::optional<std::pair<std::vector<std::string>, std::string>>
parse_build_line(std::string_view line) {
    if (!line.starts_with("build ")) return std::nullopt;
    line.remove_prefix(6);
    std::vector<std::string> outputs;
    std::string cur;
    std::size_t i = 0;
    bool done = false;
    auto flush = [&] {
        if (!cur.empty() && cur != "|") outputs.push_back(normalise(cur));
        cur.clear();
    };
    for (; i < line.size() && !done; ++i) {
        const char c = line[i];
        if (c == '$' && i + 1 < line.size()) {
            const char n = line[i + 1];
            if (n == ' ' || n == ':' || n == '$') { cur += n; ++i; continue; }
            cur += c;
            continue;
        }
        if (c == ':') { flush(); done = true; break; }
        if (c == ' ') { flush(); continue; }
        cur += c;
    }
    if (!done) return std::nullopt;
    std::string_view rest = line.substr(i + 1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    auto rule = rest.substr(0, rest.find_first_of(" \n"));
    return std::pair{std::move(outputs), std::string(rule)};
}

} // namespace

void Attribution::statement(std::string_view text) {
    while (!text.empty()) {
        auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        auto parsed = parse_build_line(line);
        if (!parsed || parsed->second == "phony" || parsed->first.empty()) continue;
        steps_.push_back({std::move(parsed->first), std::move(parsed->second), owner_});
    }
}

void Attribution::action(std::string_view firstOutput, std::string_view label) {
    actions_.emplace_back(normalise(firstOutput), std::string(label));
}

Record Attribution::record(const std::vector<PackageInfo>& declared) const {
    Record r;
    std::unordered_map<std::string, std::size_t> index;
    for (auto const& d : declared) {
        if (index.contains(d.name)) continue;
        index.emplace(d.name, r.packages.size());
        auto p = d;
        p.steps = 0;
        r.packages.push_back(std::move(p));
    }
    for (auto const& s : steps_) {
        const auto id = r.steps++;
        if (s.owner.empty()) continue;
        auto it = index.find(s.owner);
        if (it == index.end()) {
            it = index.emplace(s.owner, r.packages.size()).first;
            r.packages.push_back({s.owner, false, s.owner, 0, 0, {}, {}});
        }
        ++r.packages[it->second].steps;
        for (auto const& o : s.outputs) {
            r.owner.emplace(o, it->second);
            r.step.emplace(o, id);
        }
    }
    for (auto const& [out1, label] : actions_) r.actions.emplace(out1, label);
    return r;
}

// ─── The readers ─────────────────────────────────────────────────────────

std::optional<StatusLine> parse_status(std::string_view line) {
    constexpr std::string_view head = "\x1b[0m@@mcpp ";
    if (!line.starts_with(head)) return std::nullopt;
    line.remove_prefix(head.size());
    const auto close = line.find("@@ ");
    const auto closeEnd = close == std::string_view::npos ? line.find("@@") : close;
    if (closeEnd == std::string_view::npos) return std::nullopt;
    auto nums = line.substr(0, closeEnd);
    StatusLine s;
    s.text = close == std::string_view::npos ? std::string_view{} : line.substr(close + 3);
    std::array<std::string_view, 4> part;
    std::size_t parts = 0;
    for (; parts < 4; ++parts) {
        auto sp = nums.find(' ');
        part[parts] = nums.substr(0, sp);
        if (sp == std::string_view::npos) { ++parts; break; }
        nums.remove_prefix(sp + 1);
    }
    if (parts < 3) return std::nullopt;
    s.finished = to_size(part[0]);
    s.total    = to_size(part[1]);
    if (parts >= 4 && !part[3].empty()) s.unstarted = to_size(part[3]);
    // `%e` is seconds with three decimals: the step's end in milliseconds.
    auto e = part[2];
    auto dot = e.find('.');
    long long secs = to_ll(e.substr(0, dot));
    long long frac = 0;
    if (dot != std::string_view::npos) {
        auto f = e.substr(dot + 1);
        std::string three(f.substr(0, 3));
        while (three.size() < 3) three += '0';
        frac = to_ll(three);
    }
    s.endMs = secs * 1000 + frac;
    return s;
}

std::vector<LogStep> parse_log(std::string_view text, std::size_t* consumed, int* version,
                               std::vector<std::size_t>* offsets) {
    std::vector<LogStep> steps;
    std::size_t pos = 0;
    std::string lastHash;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) break;   // an entry still being written
        auto line = text.substr(pos, nl - pos);
        const auto lineStart = pos;
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.front() == '#') {
            if (version) {
                constexpr std::string_view v = "# ninja log v";
                if (line.starts_with(v)) *version = static_cast<int>(to_ll(line.substr(v.size())));
            }
            continue;
        }
        auto f = split_tabs(line);
        if (f.size() < 5) continue;
        const long long start = to_ll(f[0]), end = to_ll(f[1]);
        const std::string hash(f[4]);
        if (!steps.empty() && steps.back().start == start && steps.back().end == end
            && hash == lastHash) {
            steps.back().outputs.push_back(normalise(f[3]));
        } else {
            steps.push_back({start, end, {normalise(f[3])}});
            if (offsets) offsets->push_back(lineStart);
            lastHash = hash;
        }
    }
    if (consumed) *consumed = pos;
    return steps;
}

std::size_t run_boundary(const std::vector<LogStep>& steps, std::vector<long long> ends) {
    std::size_t i = steps.size();
    while (i > 0) {
        auto it = std::ranges::find(ends, steps[i - 1].end);
        if (it == ends.end()) break;
        ends.erase(it);
        --i;
    }
    return i;
}

std::vector<ActionStart> parse_starts(std::string_view text, std::size_t* consumed) {
    std::vector<ActionStart> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) break;
        auto line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        auto f = split_tabs(line);
        if (f.size() < 2 || f[0].empty()) continue;
        out.push_back({normalise(f[0]), to_ll(f[1])});
    }
    if (consumed) *consumed = pos;
    return out;
}

void record_action_start(std::string_view stamp) {
    const char* file = std::getenv(std::string(kStartsEnv).c_str());
    if (!file || !*file || stamp.empty()) return;
    const auto now = std::chrono::duration_cast<ms>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // One line, one write, in append mode: the lines of actions that start at
    // the same time do not interleave.
    (void)mcpp::platform::fs::append_atomically(
        std::filesystem::path(file), std::format("{}\t{}\n", field(stamp), now));
}

// ─── The model ───────────────────────────────────────────────────────────

// Not exported, and not TU-local either: `Build::Impl` holds them.
enum class Phase { Planning, Programs, Building, Stopping, Checking };

struct Program {
    std::string name;
    enum State { Waiting, Compiling, Running, Done } state = Waiting;
    ProgramOutcome outcome = ProgramOutcome::Ran;
    Clock::time_point since{};
    ms compile{0}, run{0};
};

struct PackageState {
    std::size_t finished  = 0;
    long long   first     = std::numeric_limits<long long>::max();   // ms since command start
    long long   last      = 0;
    bool        announced = false;   // its line was written
};

struct Running {
    std::size_t package = 0;
    std::string label;
    std::string stamp;
    long long   since = 0;   // ms since command start
};

struct Build::Impl {
    std::filesystem::path dir;
    std::vector<PackageInfo> declared;
    std::optional<Record> record;
    bool recordTried = false;
    bool closed = false;                       // the build's Build object is gone
    std::unordered_set<std::size_t> counted;   // steps already counted
    std::vector<PackageState> packages;
    // Passes of ninja.
    long long   passStart = 0;       // ms since command start
    std::size_t doneBefore = 0, totalBefore = 0;   // earlier passes
    std::size_t finished = 0, total = 0;          // this pass
    std::optional<std::size_t> unstarted;         // this pass, from `%u`
    bool        inPass = false;
    // The log of this pass.
    std::filesystem::path logPath;
    std::string logTail;             // the file's last bytes when the pass began
    std::uintmax_t logStart = 0;     // its size then
    std::optional<std::uintmax_t> logOffset;   // where this pass's entries are read from
    std::vector<long long> ends;     // end times of the status lines of this pass
    // The start file.
    std::uintmax_t startsOffset = 0;
    std::vector<Running> running;
    // The longest step, for `Finished`.
    long long   longest = 0;
    std::string longestLabel;
    bool        anyStep = false;
};

namespace {

// The fast path has no plan: it reads the record the plan wrote, when ninja
// reports its first step, so a build with nothing to do reads nothing.
void ensure_record(Build::Impl& b) {
    if (b.record || b.recordTried) return;
    b.recordTried = true;
    if (auto rec = read_record(b.dir)) {
        b.packages.assign(rec->packages.size(), PackageState{});
        b.record = std::move(*rec);
    }
}

namespace screen = mcpp::ui::dots_screen;

struct Report {
    std::mutex m;
    bool open = false;
    bool verbose = false;
    Phase phase = Phase::Planning;
    std::size_t configurations = 1;
    std::vector<Program> programs;
    ms   programTime{0};
    std::vector<std::shared_ptr<Build::Impl>> builds;
    std::optional<long long> buildStart;   // ms since command start
    bool failureReported = false;
    bool deferred = false;
    std::optional<std::pair<std::string, std::string>> deferredFinish;
    // The status row's screen (revision 3, §5.9 to §5.13): an animation fed
    // by the build, or none.
    std::unique_ptr<screen::Animation> animation;
    bool animationColour = false;
    // `--play-game` (revision 3, §5.14): the game, its name, and the keys it
    // reads. The game is also `animation`'s place on the screen.
    std::unique_ptr<screen::Game> game;
    std::string gameName;
    std::unique_ptr<mcpp::platform::terminal::KeyInput> keys;
    std::vector<screen::Source> started;   // packages announced since the last frame
    long long   lastFrame = 0;             // ms since command start
    std::size_t lastDone  = 0;
};

Report& report() {
    static Report r;
    return r;
}

long long now_ms() {
    return std::chrono::duration_cast<ms>(Clock::now() - mcpp::ui::command_start()).count();
}

constexpr std::size_t kColumnMax = 43;   // the state at column 56 at most

std::string plural(std::size_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

screen::Source source_of(std::string_view s) {
    if (s == "project")  return screen::Source::Project;
    if (s == "official") return screen::Source::Official;
    if (s == "index")    return screen::Source::Index;
    if (s == "git")      return screen::Source::Git;
    return screen::Source::Other;
}

mcpp::ui::Hue hue_of(std::string_view s) {
    if (s == "official") return mcpp::ui::Hue::Cyan;
    if (s == "index")    return mcpp::ui::Hue::Magenta;
    if (s == "git")      return mcpp::ui::Hue::Blue;
    return mcpp::ui::Hue::Plain;
}

// ── Programs ──

std::size_t program_column(const Report& r) {
    std::size_t w = 16;
    for (auto const& p : r.programs) w = std::max(w, mcpp::ui::display_width(p.name));
    return std::min(w + 2, kColumnMax);
}

std::string program_state(const Program& p, bool verbose) {
    switch (p.state) {
    case Program::Waiting:   return "waiting";
    case Program::Compiling:
        return std::format("compiling {}", mcpp::ui::format_clock(
            std::chrono::duration_cast<ms>(Clock::now() - p.since)));
    case Program::Running:
        return std::format("running {}", mcpp::ui::format_clock(
            std::chrono::duration_cast<ms>(Clock::now() - p.since)));
    case Program::Done: break;
    }
    if (p.outcome == ProgramOutcome::Cached) return "cached";
    if (p.outcome == ProgramOutcome::Failed) return "failed";
    if (verbose && p.compile.count() > 0)
        return std::format("compiled {} · ran {}", mcpp::ui::format_duration(p.compile),
                           mcpp::ui::format_duration(p.run));
    return std::format("ran {}", mcpp::ui::format_duration(p.compile + p.run));
}

mcpp::ui::Tone program_tone(const Program& p) {
    if (p.state != Program::Done) return mcpp::ui::Tone::Plain;
    if (p.outcome == ProgramOutcome::Failed) return mcpp::ui::Tone::Bad;
    if (p.outcome == ProgramOutcome::Cached) return mcpp::ui::Tone::Muted;
    return mcpp::ui::Tone::Good;
}

std::string program_line(const Report& r, const Program& p) {
    return mcpp::ui::step_line("build.mcpp", p.name, program_column(r), program_state(p, r.verbose),
                               program_tone(p), /*infoVerb=*/true);
}

Program& program(Report& r, std::string_view name) {
    for (auto& p : r.programs)
        if (p.name == name && p.state != Program::Done) return p;
    r.programs.push_back({std::string(name)});
    return r.programs.back();
}

// ── Packages ──

// The package as its line names it: the name coloured by source, the version
// and origin dim (revision 3, §5.10).
std::string subject_of(const Report& r, const Build::Impl& b, const PackageInfo& p) {
    std::string s = mcpp::ui::hue(p.subject, hue_of(p.source));
    if (!p.detail.empty()) s += " " + mcpp::ui::hue(p.detail, mcpp::ui::Hue::Dim);
    if (r.configurations > 1) s += std::format(" [{}]", b.dir.filename().string());
    return s;
}

// The same, without colour, for the error line.
std::string plain_subject(const Build::Impl& b, std::size_t i) {
    const auto& p = b.record->packages[i];
    return p.detail.empty() ? p.subject : std::format("{} {}", p.subject, p.detail);
}

// A package's line (revision 3, §7.1): `Cached` when the global cache serves
// its units, `Compiling` otherwise.
std::string package_line(const Report& r, const Build::Impl& b, std::size_t i) {
    const auto& p = b.record->packages[i];
    if (p.cachedUnits > 0) {
        // The unit count joins the origin's parentheses when there are any:
        // `v3.6.1 (index acme, 2 units)`, `v6.1.9 (73 units)`.
        auto q = p;
        const auto units = plural(p.cachedUnits, "unit", "units");
        q.detail = !q.detail.empty() && q.detail.ends_with(")")
            ? std::format("{}, {})", q.detail.substr(0, q.detail.size() - 1), units)
            : std::format("{}{}({})", q.detail, q.detail.empty() ? "" : " ", units);
        return mcpp::ui::step_line("Cached", subject_of(r, b, q), 0, "");
    }
    return mcpp::ui::step_line("Compiling", subject_of(r, b, p), 0, "");
}

// Writes a package's line the first time it does work; model lock held. The
// standard library module is the toolchain's and is not named.
void announce(Report& r, Build::Impl& b, std::size_t i, std::vector<std::string>& out) {
    auto& s = b.packages[i];
    const auto& p = b.record->packages[i];
    if (s.announced || p.name == "std" || p.name.empty()) return;
    s.announced = true;
    out.push_back(package_line(r, b, i));
    r.started.push_back(p.cachedUnits > 0 ? screen::Source::Cache : source_of(p.source));
}

void write_lines(const std::vector<std::string>& lines) {
    for (auto const& l : lines) mcpp::ui::line(l);
    if (!lines.empty()) mcpp::ui::touch_region();
}

// Reads the log of an open pass; model lock held.
void read_log(Report& r, Build::Impl& b, std::vector<std::string>& out) {
    if (!b.inPass || !b.record) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(b.logPath, ec);
    if (ec) return;
    std::ifstream f(b.logPath, std::ios::binary);
    if (!f) return;
    if (!b.logOffset) {
        // Is the file the one the pass began with? A recompaction rewrote it.
        bool same = size >= b.logStart;
        if (same && !b.logTail.empty()) {
            std::string tail(b.logTail.size(), '\0');
            f.seekg(static_cast<std::streamoff>(b.logStart - b.logTail.size()));
            f.read(tail.data(), static_cast<std::streamsize>(tail.size()));
            same = f && tail == b.logTail;
            f.clear();
        }
        if (same) b.logOffset = b.logStart;
        else {
            // This pass's entries are the suffix whose end times are those of
            // the status lines seen (design §6.2).
            f.seekg(0);
            std::string all{std::istreambuf_iterator<char>(f), {}};
            std::size_t consumed = 0;
            std::vector<std::size_t> offsets;
            auto steps = parse_log(all, &consumed, nullptr, &offsets);
            const auto first = run_boundary(steps, b.ends);
            b.logOffset = first < offsets.size() ? offsets[first] : consumed;
            f.clear();
        }
    }
    if (size <= *b.logOffset) return;
    f.seekg(static_cast<std::streamoff>(*b.logOffset));
    std::string text(static_cast<std::size_t>(size - *b.logOffset), '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    std::size_t consumed = 0;
    auto steps = parse_log(text, &consumed);
    *b.logOffset += consumed;
    for (auto const& st : steps) {
        const long long start = b.passStart + st.start, end = b.passStart + st.end;
        std::optional<std::size_t> owner;
        bool seen = false;
        for (auto const& o : st.outputs) {
            if (auto it = b.record->owner.find(o); it != b.record->owner.end()) {
                owner = it->second;
                if (auto sid = b.record->step.find(o); sid != b.record->step.end())
                    seen = !b.counted.insert(sid->second).second;
                break;
            }
        }
        // The rest of a step whose first entries an earlier read counted.
        if (seen) continue;
        std::string label = st.outputs.front();
        if (auto it = b.record->actions.find(st.outputs.front()); it != b.record->actions.end())
            label = it->second;
        if (owner) {
            auto& s = b.packages[*owner];
            ++s.finished;
            s.first = std::min(s.first, start);
            s.last  = std::max(s.last, end);
            label = std::format("{}: {}", b.record->packages[*owner].subject, label);
            // A package is named at its first step that is not a scan: a
            // dependency scan finishes before the compiles it orders, and
            // naming a package there put a consumer before the package it
            // imports. A package that only scanned is named when the pass
            // ends.
            const bool scan = std::ranges::all_of(st.outputs, [](const std::string& o) {
                return o.ends_with(".ddi") || o.ends_with(".dd");
            });
            if (!scan) announce(r, b, *owner, out);
        }
        if (st.end - st.start > b.longest || !b.anyStep) {
            b.longest = st.end - st.start;
            b.longestLabel = label;
            b.anyStep = true;
        }
        std::erase_if(b.running, [&](const Running& a) {
            return std::ranges::find(st.outputs, a.stamp) != st.outputs.end();
        });
    }
}

// Reads the start file of an open pass; model lock held. A check or prepare
// action that starts names its package: that is work.
void read_starts(Report& r, Build::Impl& b, std::vector<std::string>& out) {
    if (!b.inPass || !b.record) return;
    std::error_code ec;
    const auto path = b.dir / kStartsFile;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size <= b.startsOffset) return;
    std::ifstream f(path, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(b.startsOffset));
    std::string text(static_cast<std::size_t>(size - b.startsOffset), '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    std::size_t consumed = 0;
    auto starts = parse_starts(text, &consumed);
    b.startsOffset += consumed;
    const auto nowUnix = std::chrono::duration_cast<ms>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto const& a : starts) {
        auto owner = b.record->owner.find(a.stamp);
        if (owner == b.record->owner.end()) continue;
        auto label = b.record->actions.find(a.stamp);
        b.running.push_back({owner->second,
                             label != b.record->actions.end() ? label->second : a.stamp,
                             a.stamp, now_ms() - std::max<long long>(0, nowUnix - a.unixMs)});
        announce(r, b, owner->second, out);
    }
}

// The builds whose Build object is alive: the frame, the poll and the status
// line read these. `Finished` reads every build of the command.
std::vector<std::shared_ptr<Build::Impl>> live_builds(Report& r) {
    std::vector<std::shared_ptr<Build::Impl>> out;
    for (auto const& b : r.builds)
        if (!b->closed) out.push_back(b);
    return out;
}

// The status row (revision 3, §7.2): the phase aligned with the verbs above
// it, the screen, the counts, the clock, then what is known to be running.
std::string phase_status(Report& r, std::string_view cells) {
    const auto clock = mcpp::ui::format_clock(ms(now_ms()));
    std::string current;
    long long oldest = std::numeric_limits<long long>::max();
    std::size_t done = 0, total = 0, remaining = 0;
    bool tail = true;   // every build in a pass has started its last step
    bool anyPass = false;
    for (auto const& b : live_builds(r)) {
        done  += b->doneBefore + b->finished;
        total += b->totalBefore + b->total;
        if (b->inPass) {
            anyPass = true;
            if (!b->unstarted || *b->unstarted > 0) tail = false;
            else remaining += b->total > b->finished ? b->total - b->finished : 0;
        }
        for (auto const& a : b->running)
            if (a.since < oldest) {
                oldest  = a.since;
                current = std::format("{}: {} {}", b->record->packages[a.package].subject, a.label,
                                      mcpp::ui::format_clock(ms(now_ms() - a.since)));
            }
    }
    std::string counts;
    std::string phase;
    std::vector<std::string> extra;
    switch (r.phase) {
    case Phase::Planning: phase = "Planning"; break;
    case Phase::Programs: {
        phase = "Running";
        std::size_t finishedPrograms = 0;
        for (auto const& p : r.programs) {
            if (p.state == Program::Done) ++finishedPrograms;
            if (p.state == Program::Compiling || p.state == Program::Running)
                current = std::format("build.mcpp {} {} {}", p.name,
                    p.state == Program::Compiling ? "compiling" : "running",
                    mcpp::ui::format_clock(std::chrono::duration_cast<ms>(Clock::now() - p.since)));
        }
        counts = std::format("{}/{}", finishedPrograms, r.programs.size());
        break;
    }
    case Phase::Building:
    case Phase::Stopping:
        phase = r.phase == Phase::Building ? "Building" : "Stopping";
        if (total > 0) counts = std::format("{}/{}", done, total);
        // Once no step is left to start, every step left is running (`%u`).
        if (r.phase == Phase::Building && anyPass && tail && remaining > 0)
            extra.push_back(std::format("last {} running", remaining));
        break;
    case Phase::Checking: phase = "Checking"; break;
    }
    std::string rest(cells);
    if (!counts.empty()) rest += (rest.empty() ? "" : " ") + counts;
    rest += std::format("{}· {}", rest.empty() ? "" : " ", clock);
    for (auto const& e : extra) rest += " · " + e;
    if (!current.empty()) rest += " · " + current;
    return mcpp::ui::status_line(phase, rest);
}

mcpp::ui::Frame frame() {
    auto& r = report();
    std::lock_guard lock(r.m);
    mcpp::ui::Frame f;
    std::string cells;
    std::string score;
    if (r.game) {
        const auto now = now_ms();
        screen::Input in;
        in.dt     = r.lastFrame ? static_cast<double>(now - r.lastFrame) / 1000.0 : 0.0;
        in.failed = r.failureReported;
        for (auto s : r.started) r.game->package(s);
        r.started.clear();
        r.game->update(in);
        r.lastFrame = now;
        screen::Screen sc;
        r.game->draw(sc);
        cells = sc.render(r.animationColour);
        score = std::format("{} {}", r.gameName, r.game->score());
    } else if (r.animation) {
        std::size_t done = 0, total = 0;
        for (auto const& b : live_builds(r)) {
            done  += b->doneBefore + b->finished;
            total += b->totalBefore + b->total;
        }
        const auto now = now_ms();
        screen::Input in;
        in.dt       = r.lastFrame ? static_cast<double>(now - r.lastFrame) / 1000.0 : 0.0;
        in.finished = done > r.lastDone ? done - r.lastDone : 0;
        in.fraction = total ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : 0.0;
        in.failed   = r.failureReported;
        for (auto s : r.started) r.animation->package(s);
        r.started.clear();
        r.animation->update(in);
        r.lastFrame = now;
        r.lastDone  = done;
        // The screen takes 25 columns; a terminal narrower than 60 keeps the
        // counts and the clock instead.
        if (mcpp::platform::terminal::cols() >= 60) {
            screen::Screen sc;
            r.animation->draw(sc);
            cells = sc.render(r.animationColour);
        }
    }
    f.status = phase_status(r, cells);
    if (!score.empty()) f.status += " · " + score;
    return f;
}

void poll() {
    auto& r = report();
    std::vector<std::string> out;
    {
        std::lock_guard lock(r.m);
        if (r.game && r.keys)
            for (auto k : r.keys->read()) {
                using TK = mcpp::platform::terminal::Key;
                r.game->key(k == TK::Up   ? screen::Key::Up
                          : k == TK::Down ? screen::Key::Down
                          : k == TK::Left ? screen::Key::Left
                          : k == TK::Right ? screen::Key::Right : screen::Key::Space);
            }
        for (auto const& b : live_builds(r)) {
            read_starts(r, *b, out);
            read_log(r, *b, out);
        }
    }
    write_lines(out);
}

// The status row's screen, as MCPP_PROGRESS asks: `random` (the default) or
// an animation's name; `plain` keeps the row without a screen; `off` draws no
// live row at all. The screen needs a terminal that draws braille.
std::unique_ptr<screen::Animation> choose_animation() {
    std::string want = mcpp::platform::env::get("MCPP_PROGRESS").value_or("random");
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (want == "off") {
        mcpp::ui::set_live_progress(false);
        return nullptr;
    }
    if (want == "plain" || !mcpp::ui::live_progress()
        || !mcpp::platform::terminal::unicode_capable())
        return nullptr;
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    auto names = screen::names();
    if (auto a = screen::make(want, seed)) return a;
    return screen::make(names[seed % names.size()], seed);
}

// `--play-game[=NAME]` (revision 3, §5.14): the CLI publishes the request as
// MCPP_PLAY_GAME (`random` or a name). The game needs what the screen needs,
// and keys: standard input and standard output on a terminal. Otherwise it is
// off, and one line says why.
void choose_game(Report& r, std::vector<std::string>& notes) {
    auto want = mcpp::platform::env::get("MCPP_PLAY_GAME").value_or("");
    if (want.empty()) return;
    mcpp::platform::env::unset("MCPP_PLAY_GAME");   // not inherited by what mcpp runs
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto names = screen::game_names();
    if (want != "random" && !screen::make_game(want, 0)) {
        std::string known;
        for (auto n : names) known += (known.empty() ? "" : ", ") + std::string(n);
        notes.push_back(std::format("--play-game: no game called '{}' (the games: {}); one is chosen",
                                    want, known));
        want = "random";
    }
    const auto progress = mcpp::platform::env::get("MCPP_PROGRESS").value_or("");
    if (mcpp::ui::is_quiet() || !mcpp::ui::live_progress() || progress == "plain" || progress == "off"
        || !mcpp::platform::terminal::unicode_capable()) {
        notes.push_back("--play-game: the status row's screen is off here (a terminal that "
                        "draws braille, without --quiet and MCPP_PROGRESS=plain or off, is needed)");
        return;
    }
    auto keys = std::make_unique<mcpp::platform::terminal::KeyInput>();
    if (!keys->active()) {
        notes.push_back("--play-game: standard input is not a terminal, so no key can be read");
        return;
    }
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    r.gameName = want == "random" ? std::string(names[seed % names.size()]) : want;
    r.game = screen::make_game(r.gameName, seed);
    r.keys = std::move(keys);
    mcpp::ui::set_frame_interval(std::chrono::milliseconds(50));
}

} // namespace

void open(bool verbose) {
    auto& r = report();
    std::vector<std::string> notes;
    {
        std::lock_guard lock(r.m);
        if (r.open) return;
        r.open = true;
        r.verbose = verbose;
        choose_game(r, notes);
        if (!r.game) r.animation = choose_animation();
        r.animationColour = mcpp::ui::is_color_enabled();
    }
    for (auto const& n : notes) mcpp::ui::info("Game", n);
    mcpp::ui::open_region(&frame, &poll);
}

void close() {
    mcpp::ui::close_region();
    auto& r = report();
    std::lock_guard lock(r.m);
    r.open = false;
    r.keys.reset();   // the terminal's mode is restored here
}

void configurations(std::size_t n) {
    auto& r = report();
    std::lock_guard lock(r.m);
    r.configurations = std::max<std::size_t>(1, n);
}

void program_scheduled(std::string_view package, bool /*requested*/) {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        program(r, package);
    }
    mcpp::ui::touch_region();
}

void program_compiling(std::string_view package, bool /*requested*/) {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        r.phase = Phase::Programs;
        auto& p = program(r, package);
        p.state = Program::Compiling;
        p.since = Clock::now();
    }
    mcpp::ui::touch_region();
}

void program_running(std::string_view package, bool /*requested*/) {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        r.phase = Phase::Programs;
        auto& p = program(r, package);
        p.state = Program::Running;
        p.since = Clock::now();
    }
    mcpp::ui::touch_region();
}

void program_finished(std::string_view package, bool /*requested*/, ProgramOutcome outcome,
                      std::chrono::milliseconds compile, std::chrono::milliseconds run) {
    auto& r = report();
    std::vector<std::string> out;
    {
        std::lock_guard lock(r.m);
        auto& p = program(r, package);
        p.state   = Program::Done;
        p.outcome = outcome;
        p.compile = compile;
        p.run     = run;
        r.programTime += compile + run;
        // A program whose result is reused did no work (revision 3, §7.1).
        if (outcome != ProgramOutcome::Cached || r.verbose) out.push_back(program_line(r, p));
    }
    write_lines(out);
    mcpp::log::info("progress", std::format("build.mcpp {} {}", package,
        outcome == ProgramOutcome::Cached ? "cached"
        : outcome == ProgramOutcome::Failed ? "failed"
        : std::format("compiled {}ms ran {}ms", compile.count(), run.count())));
}

// The build programs are done and the plan continues. Measured with
// 2026.9.29.5: the status row read `Running build programs` for 13 s after the
// only program finished, because nothing returned the phase to planning.
void programs_done() {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        if (r.phase == Phase::Programs) r.phase = Phase::Planning;
    }
    mcpp::ui::touch_region();
}

void checking() {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        r.phase = Phase::Checking;
    }
    mcpp::ui::touch_region();
}

void defer_finished() {
    auto& r = report();
    std::lock_guard lock(r.m);
    r.deferred = true;
}

void finish_deferred() {
    auto& r = report();
    std::optional<std::pair<std::string, std::string>> f;
    {
        std::lock_guard lock(r.m);
        r.deferred = false;
        f = std::exchange(r.deferredFinish, std::nullopt);
    }
    if (f) finished(f->first, f->second);
}

void finished(std::string_view profile, std::string_view descriptor) {
    auto& r = report();
    std::string detail;
    const auto total = now_ms();
    {
        std::lock_guard lock(r.m);
        if (r.deferred) {
            r.deferredFinish.emplace(std::string(profile), std::string(descriptor));
            return;
        }
        // The breakdown and the longest step explain a wait; a command shorter
        // than ten seconds has none to explain (design §4.5).
        if (total >= 10'000) {
            const long long build = r.buildStart ? total - *r.buildStart : 0;
            const long long programs = r.programTime.count();
            const long long plan = std::max<long long>(0, total - build - programs);
            std::vector<std::string> parts;
            if (plan > 0)     parts.push_back("plan " + mcpp::ui::format_duration(ms(plan)));
            if (programs > 0) parts.push_back("programs " + mcpp::ui::format_duration(ms(programs)));
            if (build > 0)    parts.push_back("build " + mcpp::ui::format_duration(ms(build)));
            if (parts.size() > 1)
                for (std::size_t i = 0; i < parts.size(); ++i)
                    detail += (i ? " · " : "") + parts[i];
            long long longest = 0;
            std::string label;
            for (auto const& b : r.builds)
                if (b->anyStep && b->longest > longest) {
                    longest = b->longest;
                    label   = b->longestLabel;
                }
            if (build >= 10'000 && longest * 4 >= build && !label.empty())
                detail += std::format("{}longest {} {}", detail.empty() ? "" : " · ", label,
                                      mcpp::ui::format_duration(ms(longest)));
        }
    }
    // `Finished` ends the report: the region is erased before it, and not
    // drawn again below it.
    std::string played;
    {
        std::lock_guard lock(r.m);
        if (r.game) played = std::format("{} · best {}", r.gameName, r.game->best());
    }
    close();
    mcpp::ui::finished(profile, ms(total), descriptor, detail);
    if (!played.empty()) mcpp::ui::line(mcpp::ui::step_line("Played", played, 0, ""));
}

// ─── Build ───────────────────────────────────────────────────────────────

Build::Build(const std::filesystem::path& dir) : impl_(std::make_shared<Impl>()) {
    impl_->dir = dir;
    auto& r = report();
    std::lock_guard lock(r.m);
    r.builds.push_back(impl_);
}

// A build that ended without `finish` (the fast path's stale graph, which the
// full path then plans and builds) shows nothing from here on.
Build::~Build() {
    auto& r = report();
    std::lock_guard lock(r.m);
    impl_->closed = true;
}

void Build::declare(std::vector<PackageInfo> packages) {
    auto& r = report();
    std::lock_guard lock(r.m);
    impl_->declared = std::move(packages);
}

const std::vector<PackageInfo>& Build::declared() const { return impl_->declared; }

void Build::set_record(Record record) {
    auto& r = report();
    std::lock_guard lock(r.m);
    impl_->packages.assign(record.packages.size(), PackageState{});
    impl_->record = std::move(record);
    impl_->recordTried = true;
}

bool Build::has_record() const {
    auto& r = report();
    std::lock_guard lock(r.m);
    return impl_->record.has_value();
}

std::vector<std::pair<std::string, std::string>> Build::environment() const {
    // CLICOLOR_FORCE=0 keeps ninja stripping escape sequences from the
    // commands' output, which is what tells its own status lines from a
    // nested ninja's (see kStatusFormat).
    return {{"NINJA_STATUS", std::string(kStatusFormat)},
            {"CLICOLOR_FORCE", "0"},
            {std::string(kStartsEnv), (impl_->dir / kStartsFile).string()}};
}

void Build::pass_begin() {
    auto& r = report();
    {
        std::lock_guard lock(r.m);
        auto& b = *impl_;
        b.doneBefore  += b.finished;
        b.totalBefore += b.total;
        b.finished = b.total = 0;
        b.unstarted.reset();
        b.passStart = now_ms();
        if (!r.buildStart) r.buildStart = b.passStart;
        r.phase = Phase::Building;
        b.inPass = true;
        b.ends.clear();
        b.logPath = b.dir / ".ninja_log";
        b.logOffset.reset();
        std::error_code ec;
        b.logStart = std::filesystem::exists(b.logPath, ec)
                         ? std::filesystem::file_size(b.logPath, ec) : 0;
        if (ec) b.logStart = 0;
        b.logTail.clear();
        if (b.logStart > 0) {
            std::ifstream f(b.logPath, std::ios::binary);
            const auto n = std::min<std::uintmax_t>(b.logStart, 256);
            f.seekg(static_cast<std::streamoff>(b.logStart - n));
            b.logTail.resize(static_cast<std::size_t>(n));
            f.read(b.logTail.data(), static_cast<std::streamsize>(n));
            if (!f) b.logTail.clear();
        }
        b.running.clear();
        std::ofstream(b.dir / kStartsFile, std::ios::binary | std::ios::trunc);
        b.startsOffset = 0;
    }
    mcpp::ui::touch_region();
}

void Build::status(const StatusLine& line) {
    auto& r = report();
    std::vector<std::string> out;
    {
        std::lock_guard lock(r.m);
        auto& b = *impl_;
        ensure_record(b);
        b.finished  = line.finished;
        b.total     = line.total;
        b.unstarted = line.unstarted;
        b.ends.push_back(line.endMs);
        read_starts(r, b, out);
        read_log(r, b, out);
    }
    write_lines(out);
    mcpp::ui::touch_region();
}

std::optional<std::string> Build::failed(std::string_view outputs) {
    auto& r = report();
    std::vector<std::string> out;
    std::optional<std::string> first;
    {
        std::lock_guard lock(r.m);
        auto& b = *impl_;
        ensure_record(b);
        // `FAILED: [code=N] out1 out2 `: the step's outputs, space-separated.
        auto rest = outputs;
        if (rest.starts_with("[code=")) {
            auto close = rest.find(']');
            rest = close == std::string_view::npos ? std::string_view{} : rest.substr(close + 1);
        }
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        std::optional<std::size_t> owner;
        while (!rest.empty() && !owner) {
            auto sp = rest.find(' ');
            auto o = normalise(rest.substr(0, sp));
            if (b.record)
                if (auto it = b.record->owner.find(o); it != b.record->owner.end()) owner = it->second;
            rest.remove_prefix(sp == std::string_view::npos ? rest.size() : sp + 1);
        }
        // A failed step is not written to ninja's log, so a package whose
        // first step failed is named here.
        if (owner) announce(r, b, *owner, out);
        r.phase = Phase::Stopping;
        if (!r.failureReported)
            first = owner ? plain_subject(b, *owner) : std::string{};
        r.failureReported = true;
    }
    write_lines(out);
    return first;
}

void Build::pass_end() {
    auto& r = report();
    std::vector<std::string> out;
    {
        std::lock_guard lock(r.m);
        auto& b = *impl_;
        read_log(r, b, out);
        if (b.record)
            for (std::size_t i = 0; i < b.packages.size(); ++i)
                if (b.packages[i].finished > 0) announce(r, b, i, out);
        b.inPass = false;
        b.running.clear();
    }
    write_lines(out);
}

void Build::finish(bool success) {
    auto& r = report();
    std::vector<std::string> out;
    {
        std::lock_guard lock(r.m);
        auto& b = *impl_;
        // Under --verbose, the packages with nothing to do are named, and each
        // package that did work states its steps and the span from the start
        // of its first step to the end of its last, read from ninja's log
        // (revision 3, §7.4): the exact form of revision 2's `done <span>`.
        if (b.record && r.verbose) {
            for (std::size_t i = 0; i < b.record->packages.size(); ++i) {
                const auto& p = b.record->packages[i];
                const auto& s = b.packages[i];
                if (p.name == "std" || p.name.empty()) continue;
                if (s.finished == 0) {
                    if (success) out.push_back(mcpp::ui::step_line("Fresh", subject_of(r, b, p), 0, ""));
                    continue;
                }
                std::string span = s.first <= s.last
                    ? " · " + mcpp::ui::format_duration(ms(s.last - s.first)) : std::string{};
                out.push_back(mcpp::ui::step_line("Compiled", std::format("{} · {}{}",
                    subject_of(r, b, p), plural(s.finished, "step", "steps"), span), 0, ""));
            }
        }
        std::size_t done = 0;
        for (auto const& s : b.packages) done += s.finished;
        mcpp::log::info("progress", std::format("build {}: {} steps, {} attributed, {}",
            b.dir.filename().string(), b.doneBefore + b.finished, done,
            success ? "succeeded" : "failed"));
    }
    write_lines(out);
}

} // namespace mcpp::build::progress
