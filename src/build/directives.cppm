// mcpp.build.directives — the ONE definition of what a `build.mcpp` directive is.
//
// WHY THIS MODULE EXISTS
//
// A directive used to be defined in nine places: the Directives struct field,
// parse_line's dispatch, write_cache's emit, read_cache's parse, apply's fold
// into the manifest, cache_fresh's declared-output check, prepare.cppm's
// DirectiveMark field, markDirectiveTail, and foldDirectiveTailIntoPrivateBuild
// — plus the bundled `mcpp` module's typed wrapper. prepare.cppm's own comment
// admitted the split was still incomplete ("Link/source/fingerprint residues
// stay at the call sites"). That is the "same decision derived in N places"
// shape this codebase has paid for repeatedly (#233/#240/#242/#344): it does
// not fail when you add the directive, it fails later, somewhere else.
//
// Here a directive is ONE row in kTable. Parsing, cache serialization, cache
// deserialization, application to the manifest, the declared-output contract,
// and the private-scope fold are all driven off that row.
//
// WHY IT IS A SEPARATE MODULE RATHER THAN MORE OF build_program.cppm
//
// Not taste — a miscompile. build_program.cppm's anonymous namespace corrupts
// its own neighbours under clang 22 + C++20 modules + -O2: PR#332 established
// that an UNUSED helper added there was enough to break `contract_env`, and
// PR#334 reproduced it. mcpp.build.hostprogram was split out for exactly this
// reason and says so in its header. The rule is "stop growing that namespace",
// so the table lives here.
//
// SCOPE IS A REQUIRED FIELD, ON PURPOSE
//
// Every row must state its Scope. `include-dir` being PackagePrivate is not a
// style choice — it is the supply-chain rule that a build-time program must
// not silently widen a package's public interface (Cargo discipline). Making
// Scope a field means the next directive cannot be added without someone
// answering that question.
//
// See .agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md §4 (S5).

export module mcpp.build.directives;

import std;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;   // hash_string for the glob fingerprint
import mcpp.modgraph.glob;           // the one path-glob matcher

export namespace mcpp::build::directives {

// ── Protocol version ───────────────────────────────────────────────────────
//
// The wire version this engine speaks. The bundled `mcpp` module announces the
// version it was built against (`mcpp:protocol=<N>`) before main runs, so a
// program and the engine that compiled it always agree — the announcement only
// ever disagrees when a *cached* helper binary outlives an engine change, which
// is precisely the case worth catching.
//
// Bump when the meaning of an existing directive changes, or when a new
// directive is added that a program may rely on. An engine seeing a HIGHER
// number than this must refuse: it cannot know what it is being asked to do,
// and "warn and ignore" would turn that into a silently different build.
// v2 (#359): adds `rerun-if-changed-glob`.
inline constexpr int kProtocolVersion = 2;

// ── Cache-format epoch ─────────────────────────────────────────────────────
//
// Bump ONLY when previously written build.mcpp.cache entries become unusable —
// the record shape changed, or a directive's *interpretation* changed so that
// replaying a cached value would no longer mean what it meant when written.
// Deliberately NOT the mcpp release number: folding the whole version in would
// re-run every build program on every release for nothing. Same discipline as
// mcpp.build.cache_key::kCacheEpoch.
// Epoch 2 (#359): entries gained `glob` records. An engine that does not know
// them would replay a strict subset of the declared inputs and call a stale
// build fresh, which is exactly the silent-wrong-answer this guard exists for.
inline constexpr int kCacheEpoch = 2;

// ── Run bound ──────────────────────────────────────────────────────────────
//
// How long a build program may RUN before mcpp kills it. The compile is
// deliberately left unbounded — the same asymmetry `mcpp test` settled on
// (run 300s / build 0): a long compile is usually legitimate (a first-run std
// module build is minutes) and killing it produces a baffling failure, while a
// long-running build PROGRAM is usually stuck, and without a bound the whole
// build hangs with no diagnostic at all.
//
// MCPP_BUILD_PROGRAM_TIMEOUT overrides, in seconds; 0 disables the bound.
inline constexpr int kDefaultRunTimeoutSecs = 600;

std::chrono::milliseconds run_timeout();

// ── The table ──────────────────────────────────────────────────────────────

// Where a directive's value accumulates. One slot may be fed by several wire
// names (link-lib and link-search both produce link flags).
enum class Slot : std::size_t {
    CxxFlags = 0,
    CFlags,
    LdFlags,
    Defines,
    Generated,
    Sources,
    IncludeDirs,
    IncludeDirsAfter,
    RerunFiles,
    RerunEnv,
    // #359: an input that is a SET of files rather than one file. The
    // fingerprint is the sorted list of matching relative paths — never their
    // contents, sizes or timestamps. A program that globs (`proto/**/*.proto`)
    // otherwise cannot express "re-run me when a file appears", because no
    // declared file's hash changes and the new file is silently never built.
    RerunGlobs,
    // Build-graph nodes (`mcpp:action=`). The value is a JSON payload rather
    // than a scalar: an action has six fields, and a flat `key=value` line
    // cannot carry them. The bundled `mcpp` module owns the encoding, which
    // is exactly why the typed API is the only surface that grows (S4).
    Actions,
    Count
};
inline constexpr std::size_t kSlotCount = static_cast<std::size_t>(Slot::Count);

// Who sees the value. The field that must be answered for every new directive.
enum class Scope {
    PackagePrivate,  // only this package's own TUs — never propagated to consumers
    LinkGlobal,      // reaches the final link of whatever consumes this package
    SourceSet,       // joins the compile set
    RerunKey,        // not a build input at all; only feeds the re-run key
    GraphNode,       // declares an edge in the build graph; see manifest::BuildAction
};

// How the raw wire value is normalized before it is stored. Applied ONCE, at
// parse time, so the cache holds the already-spelled form (safe: the cache key
// hashes the compiler identity, so a dialect switch invalidates the entry
// before any old spelling could be replayed under a new dialect).
enum class Transform {
    Verbatim,
    LibFlag,        // dialect lib_flag_for  (-lfoo | foo.lib)
    LibSearchPath,  // dialect libSearchPrefix + absolute path
    DefinePrefix,   // dialect definePrefix + value
    AbsPath,        // absolute, lexically normal
};

struct Def {
    std::string_view wire;       // the `mcpp:<wire>=` name
    std::string_view tag;        // cache-record tag; empty = not persisted as a directive
    Slot             slot;
    Scope            scope;
    Transform        transform;
    // Declared-output contract: the value names a file that MUST exist after
    // the program ran, and whose disappearance invalidates the cache.
    bool             mustExistAfterRun;
    // The diagnostic when it does not, as "build.mcpp <prefix> '<path>'
    // <suffix>". Two fields rather than one generic sentence because the two
    // output-shaped directives mean genuinely different things — `generated=`
    // says "I WROTE this", `source=` says "I SELECTED this pre-existing file"
    // — and a user debugging one needs to be told which contract they broke.
    // Required (non-empty) whenever mustExistAfterRun is set.
    std::string_view missingPrefix;
    std::string_view missingSuffix;
    int              sinceProtocol;
};

inline constexpr std::array<Def, 13> kTable{{
    //  wire                    tag                  slot                    scope                  transform                must   missingPrefix                 missingSuffix                                    since
    {"cxxflag",             "cxxflag",           Slot::CxxFlags,         Scope::PackagePrivate, Transform::Verbatim,      false, "",                           "",                                              1},
    {"cflag",               "cflag",             Slot::CFlags,           Scope::PackagePrivate, Transform::Verbatim,      false, "",                           "",                                              1},
    {"link-lib",            "ldflag",            Slot::LdFlags,          Scope::LinkGlobal,     Transform::LibFlag,       false, "",                           "",                                              1},
    {"link-search",         "ldflag",            Slot::LdFlags,          Scope::LinkGlobal,     Transform::LibSearchPath, false, "",                           "",                                              1},
    {"cfg",                 "define",            Slot::Defines,          Scope::PackagePrivate, Transform::DefinePrefix,  false, "",                           "",                                              1},
    {"generated",           "generated",         Slot::Generated,        Scope::SourceSet,      Transform::Verbatim,      true,  "declared generated source",  "but it does not exist after the run",           1},
    {"source",              "source",            Slot::Sources,          Scope::SourceSet,      Transform::Verbatim,      true,  "selected source",            "(mcpp:source=) but no such file exists",         1},
    {"include-dir",         "include-dir",       Slot::IncludeDirs,      Scope::PackagePrivate, Transform::AbsPath,       false, "",                           "",                                              1},
    {"include-dir-after",   "include-dir-after", Slot::IncludeDirsAfter, Scope::PackagePrivate, Transform::AbsPath,       false, "",                           "",                                              1},
    {"rerun-if-changed",    "",                  Slot::RerunFiles,       Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              1},
    {"rerun-if-env-changed","",                  Slot::RerunEnv,         Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              1},
    {"rerun-if-changed-glob","",                 Slot::RerunGlobs,       Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              2},
    {"action",              "action",            Slot::Actions,          Scope::GraphNode,      Transform::Verbatim,      false, "",                           "",                                              1},
}};

// ── Collected output of one run ────────────────────────────────────────────

struct Directives {
    std::array<std::vector<std::string>, kSlotCount> slots{};
    // The protocol the program announced. 0 = it never announced one, which
    // means a hand-written `printf("mcpp:...")` program (the frozen surface).
    int protocol = 0;
    // `mcpp:` keys this engine does not know. Whether that is fatal depends on
    // `protocol` — see unknown_directive_error().
    std::vector<std::string> unknownKeys;

    std::vector<std::string>&       at(Slot s)       { return slots[static_cast<std::size_t>(s)]; }
    const std::vector<std::string>& at(Slot s) const { return slots[static_cast<std::size_t>(s)]; }
};

// ── Lookups ────────────────────────────────────────────────────────────────

const Def* find_by_wire(std::string_view wire);
const Def* find_by_tag(std::string_view tag);

// ── Path helper (shared with the caller's existence checks) ────────────────

std::string abs_against(const std::filesystem::path& base, std::string_view p);

// ── Parse ──────────────────────────────────────────────────────────────────

enum class LineResult {
    NotADirective,  // ordinary program chatter
    Accepted,
    Protocol,       // `mcpp:protocol=<N>`
    Unknown,        // a `mcpp:` key this engine does not know
};

// Parse ONE stdout line into `d`. `root` resolves relative paths for the
// path-shaped transforms; `dial` spells the link/define flags.
LineResult accept_line(Directives& d, const mcpp::toolchain::CommandDialect& dial,
                       const std::filesystem::path& root, std::string_view raw);

void accept_output(Directives& d, const mcpp::toolchain::CommandDialect& dial,
                   const std::filesystem::path& root, std::string_view out);

// Non-empty when the run must be rejected: either the program speaks a newer
// protocol than this engine, or it declared a protocol and still emitted a
// directive this engine does not know (inside a version both sides agree on,
// an unknown key is a bug, not a forward-compat situation).
//
// A program that never announced a protocol keeps the historical
// warn-and-ignore behaviour: it is a hand-written printf program, frozen at
// protocol 1, and its unknown keys are typos rather than future syntax.
std::optional<std::string> protocol_error(const Directives& d);

// ── Cache serialization ────────────────────────────────────────────────────

// `d <tag> <value>` lines, in table order.
void serialize(std::ostream& os, const Directives& d);

// One `d <tag> <value>` record. Returns false for an unknown tag (a cache
// written by a newer mcpp) — the caller treats that as a stale entry.
bool accept_cache_record(Directives& d, std::string_view tag, std::string_view value);

// ── Glob inputs (#359) ─────────────────────────────────────────────────────
//
// The fingerprint of `rerun-if-changed-glob=<pattern>`: the SORTED SET of
// relative paths matching the pattern under `root`, and nothing else.
//
// Deliberately not contents, size or mtime:
//   * contents are already covered — a file whose bytes matter is declared as
//     an ordinary `rerun-if-changed` input, and size is a strictly weaker
//     signal than the hash that entry already carries;
//   * mtime is unstable across git checkout, container builds and rsync, and
//     this project has already paid for treating a timestamp as identity
//     (the file_time_type epoch in the dependency cache).
// The question a glob input asks is "which files are here", so the answer is
// the path set, exactly.
//
// `root`-relative, generic_string, byte-ordered — otherwise the same tree
// fingerprints differently depending on the platform's directory-iteration
// order and separator.
//
// `outputDirName` (typically "target") and ".git" are never walked. A build
// program writes its outputs INSIDE the project, so a pattern like `**` would
// otherwise include what the previous run produced and the set would change on
// every build — a permanent re-run loop, and the classic Cargo footgun. This
// is enforced here rather than left to the author's pattern.
std::string glob_fingerprint(const std::filesystem::path& root,
                             std::string_view pattern,
                             std::string_view outputDirName);

// ── Apply ──────────────────────────────────────────────────────────────────

// Fold the collected directives into the manifest's buildConfig. The single
// place that knows which manifest channel each slot feeds.
void apply(mcpp::manifest::Manifest& m, const Directives& d);

// Decode one `mcpp:action=` JSON payload. nullopt = malformed.
std::optional<mcpp::manifest::BuildAction> decode_action(std::string_view payload);

// Non-empty when any declared action is malformed. A separate pass so the
// caller can refuse BEFORE applying anything — a half-applied action set is
// worse than none.
std::string action_error(const Directives& d);

// Resolve an action's paths against `pkgRoot` and make its Source outputs
// exist, so the ordinary source scan can see them.
//
// A placeholder rather than a synthesised CompileUnit, because that reuses
// every existing mechanism: the glob finds it, the scanner reads it, the plan
// gives it an object path, and ninja overwrites it with the real content
// before the compile edge runs (the compile depends on the action's output).
//
// For a module interface the placeholder carries the DECLARED interface —
// `export module X;` plus its imports — so the prepare-time scan agrees with
// what the generator will emit. That is the same assertion-plus-verification
// trade `[modules].scan_overrides` makes: the declaration is checked against
// the compiler's own P1689 output at build time, so a wrong one is caught
// rather than silently believed.
//
// Never truncates an existing file: after the first build the real content is
// there, and rewriting it would make ninja think the input changed on every
// prepare.
void prepare_actions(std::vector<mcpp::manifest::BuildAction>& actions,
                     const std::filesystem::path& pkgRoot);

// Does this action output belong in the COMPILE set?
//
// A `source` action routinely emits companion files that must exist but must
// not be compiled — protoc writes `foo.pb.cc` AND `foo.pb.h`, and the header
// is an include, not a translation unit. Adopting everything gave both the
// same object path and tripped the uniqueness assertion
// ("object path collision after uniqueness pass").
//
// The non-source outputs are still declared to ninja, so the edge still
// produces them and anything that includes them still waits for the generator.
bool is_compilable_output(const std::filesystem::path& p);

// ── Private-scope fold (was prepare.cppm's DirectiveMark / fold pair) ──────
//
// Lives here because "which compile-visible channels a PackagePrivate
// directive lands in" is a property of the table, not of the call site. The
// caller records a Mark before running the program and folds the tail after.

struct Mark {
    std::size_t cflags = 0, cxxflags = 0, includeDirs = 0, includeDirsAfter = 0;
};

Mark mark(const mcpp::manifest::Manifest& m);

// Fold the PackagePrivate tail into a UsageRequirements-shaped destination.
// Templated so this module does not have to import the scanner (which would
// close a module cycle) — the destination only needs the four vectors.
template <class Usage>
void fold_private_tail(Usage& dst, const mcpp::manifest::Manifest& ran, const Mark& t) {
    auto const& bc = ran.buildConfig;
    dst.cflags.insert(dst.cflags.end(),
                      bc.cflags.begin() + static_cast<std::ptrdiff_t>(t.cflags),
                      bc.cflags.end());
    dst.cxxflags.insert(dst.cxxflags.end(),
                        bc.cxxflags.begin() + static_cast<std::ptrdiff_t>(t.cxxflags),
                        bc.cxxflags.end());
    auto append_unique = [](auto& v, const std::filesystem::path& p) {
        if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    };
    for (auto it = bc.includeDirs.begin() + static_cast<std::ptrdiff_t>(t.includeDirs);
         it != bc.includeDirs.end(); ++it)
        append_unique(dst.includeDirs, *it);
    for (auto it = bc.includeDirsAfter.begin() + static_cast<std::ptrdiff_t>(t.includeDirsAfter);
         it != bc.includeDirsAfter.end(); ++it)
        append_unique(dst.includeDirsAfter, *it);
}

} // namespace mcpp::build::directives

namespace mcpp::build::directives {

namespace fs = std::filesystem;

const Def* find_by_wire(std::string_view wire) {
    for (auto const& d : kTable)
        if (d.wire == wire) return &d;
    return nullptr;
}

const Def* find_by_tag(std::string_view tag) {
    if (tag.empty()) return nullptr;
    for (auto const& d : kTable)
        if (d.tag == tag) return &d;   // first row wins; rows sharing a tag share a slot
    return nullptr;
}

std::string abs_against(const fs::path& base, std::string_view p) {
    fs::path pp(p);
    if (pp.is_relative()) pp = base / pp;
    return pp.lexically_normal().string();
}

std::chrono::milliseconds run_timeout() {
    int secs = kDefaultRunTimeoutSecs;
    if (const char* v = std::getenv("MCPP_BUILD_PROGRAM_TIMEOUT")) {
        std::string_view sv(v);
        int parsed = 0;
        if (std::from_chars(sv.data(), sv.data() + sv.size(), parsed).ec == std::errc{}
            && parsed >= 0)
            secs = parsed;
    }
    return std::chrono::milliseconds(static_cast<long long>(secs) * 1000);
}

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

std::string transformed(const Def& def, std::string_view raw,
                        const mcpp::toolchain::CommandDialect& dial,
                        const fs::path& root) {
    switch (def.transform) {
        case Transform::Verbatim:      return std::string(raw);
        case Transform::LibFlag:       return mcpp::toolchain::lib_flag_for(dial, raw);
        case Transform::LibSearchPath: return std::string(dial.libSearchPrefix)
                                            + abs_against(root, raw);
        case Transform::DefinePrefix:  return std::string(dial.definePrefix) + std::string(raw);
        case Transform::AbsPath:       return abs_against(root, raw);
    }
    return std::string(raw);
}

}  // namespace

LineResult accept_line(Directives& d, const mcpp::toolchain::CommandDialect& dial,
                       const fs::path& root, std::string_view raw) {
    std::string line = trim(raw);
    constexpr std::string_view kPfx = "mcpp:";
    if (!line.starts_with(kPfx)) return LineResult::NotADirective;
    std::string_view body = std::string_view(line).substr(kPfx.size());
    auto eq = body.find('=');
    std::string key = std::string(body.substr(0, eq));
    std::string val = eq == std::string_view::npos ? std::string()
                                                   : std::string(body.substr(eq + 1));

    if (key == "protocol") {
        int n = 0;
        auto* first = val.data();
        auto* last  = val.data() + val.size();
        if (std::from_chars(first, last, n).ec == std::errc{}) d.protocol = n;
        return LineResult::Protocol;
    }

    const Def* def = find_by_wire(key);
    if (!def) {
        if (std::find(d.unknownKeys.begin(), d.unknownKeys.end(), key)
            == d.unknownKeys.end())
            d.unknownKeys.push_back(key);
        return LineResult::Unknown;
    }
    d.at(def->slot).push_back(transformed(*def, val, dial, root));
    return LineResult::Accepted;
}

void accept_output(Directives& d, const mcpp::toolchain::CommandDialect& dial,
                   const fs::path& root, std::string_view out) {
    std::size_t pos = 0;
    while (pos <= out.size()) {
        std::size_t nl = out.find('\n', pos);
        std::string_view ln = out.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        accept_line(d, dial, root, ln);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
}

std::optional<std::string> protocol_error(const Directives& d) {
    if (d.protocol > kProtocolVersion) {
        return std::format(
            "build.mcpp speaks directive protocol {}, but this mcpp understands "
            "at most {}.\n"
            "       The package was written for a newer mcpp — upgrade with "
            "`mcpp self update`.\n"
            "       (Continuing would silently drop directives this build "
            "depends on.)",
            d.protocol, kProtocolVersion);
    }
    if (d.protocol > 0 && !d.unknownKeys.empty()) {
        std::string list;
        for (auto const& k : d.unknownKeys)
            list += (list.empty() ? "" : ", ") + ("mcpp:" + k);
        return std::format(
            "build.mcpp emitted directive(s) this mcpp does not know: {}.\n"
            "       The program announced protocol {}, which this mcpp also "
            "speaks, so an unrecognized directive is a typo rather than newer "
            "syntax.",
            list, d.protocol);
    }
    return std::nullopt;
}

void serialize(std::ostream& os, const Directives& d) {
    // Table order, and one pass per row rather than per slot: rows sharing a
    // slot (link-lib / link-search) share a tag, so emitting per row would
    // duplicate them.
    std::array<bool, kSlotCount> done{};
    for (auto const& def : kTable) {
        if (def.tag.empty()) continue;
        auto idx = static_cast<std::size_t>(def.slot);
        if (done[idx]) continue;
        done[idx] = true;
        for (auto const& v : d.at(def.slot))
            os << "d " << def.tag << ' ' << v << '\n';
    }
}

bool accept_cache_record(Directives& d, std::string_view tag, std::string_view value) {
    const Def* def = find_by_tag(tag);
    if (!def) return false;
    d.at(def->slot).emplace_back(value);
    return true;
}

std::string glob_fingerprint(const std::filesystem::path& root,
                             std::string_view pattern,
                             std::string_view outputDirName) {
    namespace fs = std::filesystem;
    std::vector<std::string> hits;
    std::error_code ec;
    // skip_permission_denied only: symlinked directories are NOT followed, the
    // same rule the source scan uses, so a self-referential link cannot make
    // this walk diverge.
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return {};
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        std::error_code dec;
        if (it->is_directory(dec)) {
            auto name = p.filename().string();
            if (name == ".git" || (!outputDirName.empty() && name == outputDirName)) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink(dec)) it.disable_recursion_pending();
            continue;
        }
        if (!mcpp::modgraph::path_matches_glob(p, root, pattern)) continue;
        std::string rel;
        try {
            rel = p.lexically_relative(root).generic_string();
        } catch (const std::exception&) {
            continue;   // unspellable name — see path_matches_glob
        }
        hits.push_back(std::move(rel));
    }
    std::ranges::sort(hits);
    std::string joined;
    for (auto const& h : hits) { joined += h; joined.push_back('\n'); }
    return mcpp::toolchain::hash_string(joined);
}

void apply(mcpp::manifest::Manifest& m, const Directives& d) {
    auto& bc = m.buildConfig;
    auto const& cxx      = d.at(Slot::CxxFlags);
    auto const& c        = d.at(Slot::CFlags);
    auto const& ld       = d.at(Slot::LdFlags);
    auto const& defines  = d.at(Slot::Defines);

    bc.cxxflags.insert(bc.cxxflags.end(), cxx.begin(), cxx.end());
    bc.cflags.insert(bc.cflags.end(), c.begin(), c.end());
    bc.ldflags.insert(bc.ldflags.end(), ld.begin(), ld.end());
    // cfg defines colour BOTH language channels — the one slot that fans out.
    bc.cflags.insert(bc.cflags.end(), defines.begin(), defines.end());
    bc.cxxflags.insert(bc.cxxflags.end(), defines.begin(), defines.end());

    // Generated + selected sources join the source set. BOTH lists: the
    // scanner walks the legacy modules.sources mirror, so pushing only
    // bc.sources leaves a generated file outside the base globs invisible to
    // the scan (latent since L3).
    for (auto slot : {Slot::Generated, Slot::Sources}) {
        for (auto const& s : d.at(slot)) {
            bc.sources.push_back(s);
            m.modules.sources.push_back(s);
        }
    }

    // Already absolute from the AbsPath transform. PRIVATE by design: for the
    // root these join buildConfig before the package snapshot; for a
    // dependency the caller mirrors them into privateBuild only, never into
    // publicUsage.
    for (auto const& p : d.at(Slot::IncludeDirs))
        bc.includeDirs.emplace_back(p);
    for (auto const& p : d.at(Slot::IncludeDirsAfter))
        bc.includeDirsAfter.emplace_back(p);

    // Build-graph nodes. Decoded here rather than at parse time so the cache
    // stores the payload verbatim and a replay is byte-identical to a run.
    for (auto const& payload : d.at(Slot::Actions)) {
        if (auto a = decode_action(payload)) bc.actions.push_back(std::move(*a));
    }
}

std::optional<mcpp::manifest::BuildAction> decode_action(std::string_view payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        mcpp::manifest::BuildAction a;
        a.id = j.value("id", std::string{});
        auto role = j.value("role", std::string{"source"});
        a.role = role == "check"    ? mcpp::manifest::BuildAction::Role::Check
               : role == "artifact" ? mcpp::manifest::BuildAction::Role::Artifact
                                    : mcpp::manifest::BuildAction::Role::Source;
        auto arr = [&](const char* k, std::vector<std::string>& dst) {
            if (auto it = j.find(k); it != j.end() && it->is_array())
                for (auto const& v : *it)
                    if (v.is_string()) dst.push_back(v.get<std::string>());
        };
        arr("inputs", a.inputs);
        arr("outputs", a.outputs);
        arr("command", a.command);
        arr("provides", a.provides);
        arr("imports", a.imports);
        a.blocking    = j.value("blocking", false);
        a.description = j.value("description", std::string{});
        if (a.command.empty() || a.outputs.empty()) return std::nullopt;
        if (a.id.empty()) a.id = a.outputs.front();
        return a;
    } catch (...) {
        return std::nullopt;
    }
}

std::string action_error(const Directives& d) {
    for (auto const& payload : d.at(Slot::Actions)) {
        // The typed API sets this when an argv did not fit its fixed buffer.
        // Diagnosed separately because "malformed action" would send the
        // author looking for a typo in something that was actually correct
        // and merely too long.
        if (payload.find("\"overflow\":true") != std::string::npos) {
            return std::format(
                "build.mcpp declared an action whose arguments did not fit.\n"
                "       The typed `mcpp::action` builder uses fixed buffers "
                "(the bundled module has to stay\n"
                "       buildable before a std module exists, so it cannot use "
                "std::string).\n"
                "       Shorten the command — e.g. pass a response file, or a "
                "directory instead of\n"
                "       enumerating its files.\n"
                "       payload: {}", payload);
        }
        if (decode_action(payload)) continue;
        // A malformed action is a hard error, never a skip: an action that
        // silently does not exist produces a build missing generated sources,
        // and the user is left staring at a "no such file" three edges away.
        return std::format(
            "build.mcpp declared a malformed action.\n"
            "       Every action needs a non-empty `command` and at least one\n"
            "       declared `output` — mcpp fixes the source set during prepare,\n"
            "       so an output whose NAME is unknown cannot be built.\n"
            "       payload: {}", payload);
    }
    return {};
}

bool is_compilable_output(const fs::path& p) {
    auto ext = p.extension().string();
    // The same set the plan treats as translation units, plus .cppm/.ixx for a
    // generated module interface.
    return ext == ".cpp" || ext == ".cc"  || ext == ".cxx" || ext == ".c"
        || ext == ".m"   || ext == ".mm"
        || ext == ".S"   || ext == ".s"   || ext == ".asm"
        || ext == ".cppm" || ext == ".ixx";
}

void prepare_actions(std::vector<mcpp::manifest::BuildAction>& actions,
                     const fs::path& pkgRoot) {
    for (auto& a : actions) {
        auto absolutize = [&](std::vector<std::string>& v) {
            for (auto& p : v) {
                // An engine variable is resolved later, once the plan exists
                // (outputDir depends on the fingerprint). Leave it alone.
                if (p.find("${mcpp.") != std::string::npos) continue;
                p = abs_against(pkgRoot, p);
            }
        };
        absolutize(a.inputs);
        absolutize(a.outputs);
        if (a.role != mcpp::manifest::BuildAction::Role::Source) continue;
        for (auto const& o : a.outputs) {
            if (o.find("${mcpp.") != std::string::npos) continue;
            std::error_code ec;
            fs::path p(o);
            if (fs::exists(p, ec)) continue;      // real content already there
            fs::create_directories(p.parent_path(), ec);
            std::ofstream os(p, std::ios::trunc);
            if (!os) continue;
            if (!a.provides.empty()) {
                os << "// placeholder — replaced by action '" << a.id
                   << "' during the build\n";
                for (auto const& imp : a.imports) os << "import " << imp << ";\n";
                os << "export module " << a.provides.front() << ";\n";
            }
            // A non-module output needs nothing: an empty TU scans as
            // "provides nothing, imports nothing", which is what a plain
            // generated .cpp/.cc is.
        }
    }
}

Mark mark(const mcpp::manifest::Manifest& m) {
    return Mark{ m.buildConfig.cflags.size(),
                 m.buildConfig.cxxflags.size(),
                 m.buildConfig.includeDirs.size(),
                 m.buildConfig.includeDirsAfter.size() };
}

} // namespace mcpp::build::directives
