// mcpp.diag — the single sink for user-visible warnings and degradations.
//
// Why this exists: the engine has many branches that do LESS work because a
// precondition was not met (a toolchain lacks a capability, a payload is
// missing, a platform has no equivalent mechanism). Historically those
// branches were silent, or logged at debug level, which is the same thing
// from a user's point of view. The batch invariant is:
//
//   Any branch that does less because a condition was not met MUST either
//   return an error or report through diag::degraded(). log::debug and
//   log::verbose do NOT count as user-visible.
//
// A `degraded` record therefore requires an `impact` string: the author is
// forced to answer "what will the user actually experience?" — the sentence
// that was missing from every silent-degradation bug this channel was
// introduced to prevent.
//
// Ordinary `warning` records (author mistakes, schema drift) carry no impact.
//
// Records are deduplicated and rendered once, at flush(). `--strict` promotes
// degradations to errors in ONE place, replacing the per-site copies of that
// policy that had accumulated across prepare.cppm.
//
// ONE STATEMENT PER FACT PER RUN, ATTRIBUTED TO ITS SOURCE (P4 of the
// 2026-09-28 design, WS3). A `--workspace` build plans every member as a root
// and flushes after each, so a fact that holds for all of them -- a redundant
// word the workspace states, a directory that ships the C++ runtime -- was
// printed once per member: five times for five members. The terminal now
// prints each (domain, text) once per PROCESS; the per-run record, which
// `--strict` counts and `take()` hands to a machine-readable envelope, still
// holds every occurrence, so a machine reader sees each member's.

export module mcpp.diag;

import std;
import mcpp.log;
import mcpp.notice;
import mcpp.surface;   // a registered notice is an item of the surface
import mcpp.ui;

export namespace mcpp::diag {

enum class Severity { Warning, Degraded, Note };

struct Record {
    Severity    severity = Severity::Warning;
    std::string domain;   // "build/depfile", "manifest/target-cfg", ...
    std::string what;     // what happened
    std::string impact;   // consequence for the user (required for Degraded)
    std::string hint;     // optional: what to do about it
    // The object of a notice whose domain has a policy (mcpp.notice): a package, a file, a tool.
    std::string subject;
    // Whether the terminal was shown it: false for a `once` notice the project was already shown
    // and for a note under --quiet. The record is kept either way, for the machine envelope.
    bool        shown = true;

    // Rendered form, without the "warning: " / "error: " prefix.
    std::string format() const;
    // The machine code of its domain: `build/msvc-crt-word` is `MCPP_BUILD_MSVC_CRT_WORD`.
    std::string code() const;
};

// The engine did less than asked because a precondition was not met.
// `impact` is mandatory — see the module comment.
void degraded(std::string_view domain, std::string_view what,
              std::string_view impact, std::string_view hint = {});

// An author-facing problem that does not change what the engine does.
void warning(std::string_view domain, std::string_view what,
             std::string_view hint = {});

// A statement that changes nothing the build does and that a reader may want
// to act on: a dependency that ships the compiler's runtime, a newer runtime
// set chosen over the toolset's. Printed as `note:`, never promoted by
// `--strict`.
void note(std::string_view domain, std::string_view what);

// A diagnostic whose severity arrives as data: a build program's structured
// diagnostic (#734 E11), stated in the engine's own form. Rendered and recorded
// exactly as the three calls above would, so `--strict` and the JSON stream
// treat a plugin's diagnostic as they treat the engine's.
void report(Severity severity, std::string_view domain, std::string_view what,
            std::string_view impact, std::string_view hint);

// The `target/` directory of the project whose record `once` notices consult (mcpp.notice). The
// first project of a process is kept: a sub-build of the same command does not replace it. Unset
// (a command outside a project), a `once` notice is said as an `always` one is. `writes`: the
// command may write the project. A command that plans in a directory of its own (the build
// database, doctor, publish's plan) reads the record and leaves it unchanged; what it says is
// kept for the rest of the process only.
void set_project_target(const std::filesystem::path& targetDir, bool writes);

// A notice about `subject`, said by the policy of its domain: a domain with no row in
// mcpp.notice is said every time; a `once` domain once per project and subject, and again after
// `mcpp clean`. Under --verbose one already said is printed again, marked `(noted before)`.
void notice(Severity severity, std::string_view domain, std::string_view subject,
            std::string_view what, std::string_view hint = {});

// One notice about several subjects (the packages of a workspace, the files a program read): a
// `once` domain names only the subjects the project has not been told of, each marked told, and
// `what` composes the text from their phrase (`a, b and c`, mcpp.notice). Told of all of them,
// nothing is printed and the record is kept for the envelope. `scope` is part of each subject's
// key and not of the text: the package whose program read a file, the usage a package has.
void notice(Severity severity, std::string_view domain, std::string_view scope,
            std::span<const std::string> subjects,
            const std::function<std::string(std::string_view phrase)>& what,
            std::string_view hint = {});

// A fact the project's notices compare against (the default toolchain it last built with), and
// the value to keep for the next run. Without a project there is no fact to read or keep.
std::optional<std::string> project_fact(std::string_view name);
void remember_project_fact(std::string_view name, std::string_view value);

// A notice about the run as a whole rather than the step that noticed it (a refreshed index that
// requires a newer mcpp): said once, after the command's own output, as a `tip:` line, so it is
// the last thing a reader sees and never interleaves with progress. It never changes the exit
// status.
void closing(std::string_view domain, std::string_view what);
// A closing notice that belongs to a failed run only: advice that would be noise after a success
// (a newer platform release in the index).
void on_failure(std::string_view domain, std::string_view what);
// The run's closing notices, taken: a command that writes a machine-readable envelope reports
// them as notes, and they are not said again at exit.
std::vector<Record> take_closing();
// Says the closing notices, and the failure ones when `failed`, then clears both. Nothing under
// --quiet.
void print_closing(bool failed = false);

// The records of this run, cleared: what a command that writes an envelope
// reports for one member before planning the next. What was printed stays
// printed -- the once-per-process rule is about the terminal.
std::vector<Record> take();

// Records render as they are reported (see the implementation note), so this
// only settles the --strict policy and clears the run's state. Returns false
// when `strict` is set and at least one Degraded was recorded, meaning the
// caller should fail the command.
[[nodiscard]] bool flush(bool strict);

// Introspection for tests.
std::size_t count(Severity severity);
std::vector<Record> records();
void reset();

} // namespace mcpp::diag

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::diag {
namespace {

std::vector<Record> g_records;
// Every (domain, text) printed by this process. Not cleared by flush(): a
// workspace build flushes once per member, and the terminal owes the reader
// one statement per fact, not one per member (WS3).
std::set<std::string> g_printed;
std::vector<Record> g_closing;  // closing notices, in the order they were reported
std::vector<Record> g_failure;  // closing notices of a failed run only
std::optional<mcpp::notice::Record> g_project;  // the project's record of notices, once loaded
std::filesystem::path g_projectTarget;
bool g_projectWrites = false;  // the record may be saved (set_project_target)
std::mutex g_projectLock;

// Identity for deduplication: the whole payload. Two sites reporting the
// same degradation for the same reason are one record; the same domain with
// a different impact stays two, because they tell the user different things.
std::string dedup_key(const Record& r) {
    return std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}",
                       static_cast<int>(r.severity), r.domain, r.what,
                       r.impact, r.hint);
}

// The subjects of a `once` domain this project has not been told of, marked told when `mark`;
// every subject for another domain, or without a project: there is nothing to have been told.
std::vector<std::string> untold(std::string_view domain, std::string_view scope,
                                std::span<const std::string> subjects, bool mark) {
    auto key = [&](const std::string& s) {
        return mcpp::notice::key(domain, scope.empty() ? s : std::format("{}\x1f{}", scope, s));
    };
    std::vector<std::string> out(subjects.begin(), subjects.end());
    if (mcpp::notice::policy_of(domain) != mcpp::notice::Policy::Once) return out;
    std::lock_guard lock(g_projectLock);
    if (g_projectTarget.empty()) return out;
    if (!g_project)
        g_project = mcpp::notice::Record::load(g_projectTarget / mcpp::notice::kRecordFile);
    std::erase_if(out, [&](const std::string& s) { return g_project->shown(key(s)); });
    if (mark && !out.empty()) {
        for (auto const& s : out) g_project->mark(key(s));
        if (g_projectWrites) (void)g_project->save();
    }
    return out;
}

// Records render as they are pushed, not at flush(): the CLI interleaves
// warnings with `ui::status` progress lines, and deferring them would both
// reorder that stream and lose everything reported before an early return.
// Deduplication happens here, so a repeated report renders once. `toldBefore`:
// the project was told of every subject already (decided by the caller, which
// composed the text from the subjects it was not told of); unset, it is decided
// here from `r.subject`.
void push(Record r, std::optional<bool> toldBefore = std::nullopt) {
    if (mcpp::notice::find(r.domain)) mcpp::surface::exercised(mcpp::surface::kNotice, r.domain);
    auto key = dedup_key(r);
    for (auto const& existing : g_records)
        if (dedup_key(existing) == key) return;
    if (g_printed.insert(key).second) {
        // A note --quiet hides is not marked: the project has not been told it.
        const bool quietNote = r.severity == Severity::Note && mcpp::ui::is_quiet();
        const bool before = toldBefore ? *toldBefore
            : !quietNote && !r.subject.empty()
              && untold(r.domain, {}, std::span(&r.subject, 1), /*mark=*/true).empty();
        r.shown = !before && !quietNote;
        if (before && mcpp::log::is_verbose()) {
            // Marked on its first line, the statement; a `help:` line follows it.
            auto text = r.format();
            text.insert(std::min(text.find('\n'), text.size()), " (noted before)");
            mcpp::ui::note(text);
        } else if (r.shown) {
            if (r.severity == Severity::Note) mcpp::ui::note(r.format());
            else                              mcpp::ui::warning(r.format());
        }
    }
    g_records.push_back(std::move(r));
}

} // namespace

std::string Record::format() const {
    // The first line stays exactly the `what` text: existing e2e assertions
    // grep for those substrings, and a warning should read as one sentence
    // before any elaboration.
    std::string out(what);
    if (!impact.empty()) out += std::format("\n  impact: {}", impact);
    if (!hint.empty())   out += std::format("\n  help: {}", hint);
    return out;
}

void degraded(std::string_view domain, std::string_view what,
              std::string_view impact, std::string_view hint) {
    push(Record{Severity::Degraded, std::string(domain), std::string(what),
                std::string(impact), std::string(hint)});
}

void warning(std::string_view domain, std::string_view what,
             std::string_view hint) {
    push(Record{Severity::Warning, std::string(domain), std::string(what),
                std::string{}, std::string(hint)});
}

void report(Severity severity, std::string_view domain, std::string_view what,
            std::string_view impact, std::string_view hint) {
    push(Record{severity, std::string(domain), std::string(what),
                std::string(impact), std::string(hint)});
}

void note(std::string_view domain, std::string_view what) {
    push(Record{Severity::Note, std::string(domain), std::string(what),
                std::string{}, std::string{}});
}

std::string Record::code() const {
    std::string out = "MCPP_";
    for (char c : domain)
        out += (c == '/' || c == '-') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

namespace {
void add_closing(std::vector<Record>& to, std::string_view domain, std::string_view what) {
    for (auto const& r : to)
        if (r.what == what) return;
    to.push_back(Record{Severity::Note, std::string(domain), std::string(what), {}, {}});
}
}  // namespace

void closing(std::string_view domain, std::string_view what) { add_closing(g_closing, domain, what); }
void on_failure(std::string_view domain, std::string_view what) { add_closing(g_failure, domain, what); }

std::vector<Record> take_closing() { return std::exchange(g_closing, {}); }

void print_closing(bool failed) {
    auto notices = take_closing();
    auto onFailure = std::exchange(g_failure, {});
    if (failed) notices.insert(notices.end(), onFailure.begin(), onFailure.end());
    if (mcpp::ui::is_quiet()) return;
    for (auto const& n : notices) mcpp::ui::tip(n.what);
}

void set_project_target(const std::filesystem::path& targetDir, bool writes) {
    std::lock_guard lock(g_projectLock);
    if (!g_projectTarget.empty()) return;
    g_projectTarget = targetDir;
    g_projectWrites = writes;
}

void notice(Severity severity, std::string_view domain, std::string_view subject,
            std::string_view what, std::string_view hint) {
    Record r{severity, std::string(domain), std::string(what), std::string{}, std::string(hint)};
    r.subject = std::string(subject);
    push(std::move(r));
}

void notice(Severity severity, std::string_view domain, std::string_view scope,
            std::span<const std::string> subjects,
            const std::function<std::string(std::string_view phrase)>& what, std::string_view hint) {
    if (subjects.empty()) return;
    const bool quietNote = severity == Severity::Note && mcpp::ui::is_quiet();
    const auto fresh = untold(domain, scope, subjects, /*mark=*/!quietNote);
    const std::span<const std::string> named = fresh.empty() ? subjects : std::span(fresh);
    Record r{severity, std::string(domain), what(mcpp::notice::subjects(named)), std::string{},
             std::string(hint)};
    r.subject = mcpp::notice::subjects(subjects, subjects.size());
    push(std::move(r), /*toldBefore=*/fresh.empty());
}

std::optional<std::string> project_fact(std::string_view name) {
    std::lock_guard lock(g_projectLock);
    if (g_projectTarget.empty()) return std::nullopt;
    if (!g_project)
        g_project = mcpp::notice::Record::load(g_projectTarget / mcpp::notice::kRecordFile);
    return g_project->fact(name);
}

void remember_project_fact(std::string_view name, std::string_view value) {
    std::lock_guard lock(g_projectLock);
    if (g_projectTarget.empty()) return;
    if (!g_project)
        g_project = mcpp::notice::Record::load(g_projectTarget / mcpp::notice::kRecordFile);
    if (g_project->fact(name) == std::optional<std::string>(std::string(value))) return;
    g_project->set_fact(std::string(name), std::string(value));
    if (g_projectWrites) (void)g_project->save();
}

std::vector<Record> take() {
    auto out = std::move(g_records);
    g_records.clear();
    return out;
}

bool flush(bool strict) {
    const bool ok = !(strict && count(Severity::Degraded) > 0);
    if (!ok) {
        mcpp::ui::error(std::format(
            "{} degradation(s) reported and --strict is set — see the warnings "
            "above", count(Severity::Degraded)));
    }
    g_records.clear();
    return ok;
}

std::size_t count(Severity severity) {
    return static_cast<std::size_t>(std::ranges::count_if(
        g_records, [severity](const Record& r) { return r.severity == severity; }));
}

std::vector<Record> records() { return g_records; }

void reset() {
    g_records.clear();
    g_printed.clear();
    g_closing.clear();
    g_failure.clear();
    std::lock_guard lock(g_projectLock);
    g_project.reset();
    g_projectTarget.clear();
    g_projectWrites = false;
}

} // namespace mcpp::diag
