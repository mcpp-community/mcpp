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

export module mcpp.diag;

import std;
import mcpp.ui;

export namespace mcpp::diag {

enum class Severity { Warning, Degraded };

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
    mcpp::ui::warning(r.format());
    g_records.push_back(std::move(r));
}

} // namespace

std::string Record::format() const {
    // The first line stays exactly the `what` text: existing e2e assertions
    // grep for those substrings, and a warning should read as one sentence
    // before any elaboration.
    std::string out(what);
    if (!impact.empty()) out += std::format("\n  impact: {}", impact);
    if (!hint.empty())   out += std::format("\n  hint: {}", hint);
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

void reset() { g_records.clear(); }

} // namespace mcpp::diag
