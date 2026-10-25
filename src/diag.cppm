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
import mcpp.ui;

export namespace mcpp::diag {

enum class Severity { Warning, Degraded, Note };

struct Record {
    Severity    severity = Severity::Warning;
    std::string domain;   // "build/depfile", "manifest/target-cfg", ...
    std::string what;     // what happened
    std::string impact;   // consequence for the user (required for Degraded)
    std::string hint;     // optional: what to do about it

    // Rendered form, without the "warning: " / "error: " prefix.
    std::string format() const;
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

// Identity for deduplication: the whole payload. Two sites reporting the
// same degradation for the same reason are one record; the same domain with
// a different impact stays two, because they tell the user different things.
std::string dedup_key(const Record& r) {
    return std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}",
                       static_cast<int>(r.severity), r.domain, r.what,
                       r.impact, r.hint);
}

// Records render as they are pushed, not at flush(): the CLI interleaves
// warnings with `ui::status` progress lines, and deferring them would both
// reorder that stream and lose everything reported before an early return.
// Deduplication happens here, so a repeated report renders once.
void push(Record r) {
    auto key = dedup_key(r);
    for (auto const& existing : g_records)
        if (dedup_key(existing) == key) return;
    if (g_printed.insert(key).second) {
        if (r.severity == Severity::Note) mcpp::ui::note(r.format());
        else                              mcpp::ui::warning(r.format());
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

void reset() { g_records.clear(); g_printed.clear(); }

} // namespace mcpp::diag
