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
import mcpp.build.program_protocol;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;   // hash_string for the glob fingerprint
import mcpp.modgraph.glob;           // the one path-glob matcher

export namespace mcpp::build::directives {

// ── Protocol terms ─────────────────────────────────────────────────────────
//
// The protocol version, the cache epoch and the run bound have moved to
// `mcpp.build.program_protocol`. They are the terms both sides agree on BEFORE
// any directive is exchanged, they have consumers that need nothing else from
// this file (hostprogram stamps the version; build_program applies the bound),
// and keeping them here meant importing the whole directive table to ask one
// number.
//
// Re-exported under the old names so existing readers (`dirs::kCacheEpoch` in
// build_program.cppm, `dirs::kProtocolVersion` in the tests) keep working —
// this is one contract seen through two namespaces, not two contracts.
using mcpp::build::program_protocol::kProtocolVersion;
using mcpp::build::program_protocol::kCacheEpoch;
using mcpp::build::program_protocol::kDefaultRunTimeoutSecs;
using mcpp::build::program_protocol::env_timeout_override;
using mcpp::build::program_protocol::run_timeout;
using mcpp::build::program_protocol::run_timeout_for;

// ── The table ──────────────────────────────────────────────────────────────

// Where a directive's value accumulates. One slot may be fed by several wire
// names (link-lib and link-search both produce link flags).
enum class Slot : std::size_t {
    // How to EXECUTE the artifact. Its own slot, not a corner of LdFlags: it
    // is neither a compile input nor a link input, and putting it in LdFlags
    // would put an emulator's argv on the linker command line.
    Runner,
    // ⭐⭐ THE THREE SIBLINGS OF `Runner`, AND THE COLUMN THAT SEPARATES THEM.
    //
    // Writing an artefact to a device, watching what it prints and attaching a
    // debugger have `Runner`'s shape exactly: an argv the BOARD knows and a
    // TOOL performs. They are slots for the same reason `Runner` is one — an
    // emulator's argv is neither a compile input nor a link input.
    //
    // ⚠️ WHAT NO ARGV CAN SAY IS WHICH ONE ENDS. `Runner` and `Flash` finish
    // and hand back an exit code; `Monitor` and `Debug` do not terminate on
    // their own, so for them a live process IS the success condition and for
    // the other two it is a hang. `semantics_of` below answers that from the
    // SLOT, because the tokens cannot.
    Flash,
    Monitor,
    Debug,
    // Not an argv at all: a board stating that it is a mutex. See
    // `BuildConfig::runnerExclusive`.
    RunnerExclusive,
    CxxFlags,
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
    // A sentence for the USER. Not a build input at all — see Scope::Advisory
    // for why this could not be folded into any existing slot.
    Warnings,
    Count
};
inline constexpr std::size_t kSlotCount = static_cast<std::size_t>(Slot::Count);

// ⭐⭐ HOW A DEVICE ACTION'S PROCESS ENDS, WHICH IS A PROPERTY OF THE SLOT.
//
// The four device slots share an argv shape and differ in exactly one way that
// the engine has to act on: whether the process is expected to terminate.
//
//   OneShot    `run`, `flash` — runs to completion; the exit code is the verdict
//   LongLived  `monitor`, `debug` — has no natural end; the operator ends it,
//              and a non-zero status after Ctrl-C is that, not a failure
//
// ⚠️ NO TOKEN IN THE TEMPLATE CARRIES THIS. `openocd -c "program {} verify
// reset exit"` terminates and `openocd -c "init"` does not, and both are
// spelled the same way up to the argument the board chose. So it is read from
// the slot, and a board cannot get it wrong by writing its argv differently.
//
// ⚠️ AND `debug` IS `LongLived` RATHER THAN A THIRD VALUE. It starts a GDB
// SERVER; the client that attaches to it is the user's debugger or their IDE,
// which reaches mcpp through the machine-output protocol (docs/11) and not
// through this table. Driving the client would put mcpp in the middle of a
// session it has nothing to add to.
enum class Semantics { OneShot, LongLived };

inline constexpr Semantics semantics_of(Slot s) {
    return (s == Slot::Monitor || s == Slot::Debug) ? Semantics::LongLived
                                                    : Semantics::OneShot;
}

// The device slots, in the order a user meets them. Iterated rather than
// hand-listed wherever all four must be handled, so a fifth cannot be added to
// one site and missed at another.
inline constexpr Slot kDeviceSlots[] = { Slot::Runner, Slot::Flash,
                                         Slot::Monitor, Slot::Debug };

// The user-facing name of a device slot: the `mcpp <name>` subcommand, the
// `[target.<triple>].<name>` key and the `mcpp:<name>=` directive are all this
// one string, which is why it has a single read point.
inline constexpr std::string_view device_slot_name(Slot s) {
    switch (s) {
        case Slot::Runner:  return "runner";
        case Slot::Flash:   return "flash";
        case Slot::Monitor: return "monitor";
        case Slot::Debug:   return "debug";
        default:            return {};
    }
}

// Who sees the value. The field that must be answered for every new directive.
enum class Scope {
    PackagePrivate,  // only this package's own TUs — never propagated to consumers
    LinkGlobal,      // reaches the final link of whatever consumes this package
    // Reaches how the consumer RUNS the artifact. Parallel to LinkGlobal in
    // propagation and deliberately NOT the same value: the two have different
    // conflict rules. Link flags from two dependencies concatenate and that is
    // correct; two runners cannot, so this scope carries an exactly-one-
    // provider check that LinkGlobal must not inherit.
    RunGlobal,
    SourceSet,       // joins the compile set
    RerunKey,        // not a build input at all; only feeds the re-run key
    GraphNode,       // declares an edge in the build graph; see manifest::BuildAction
    // ⚠️ REACHES THE USER RATHER THAN THE BUILD, AND THAT IS WHY IT IS A
    // SEVENTH VALUE RATHER THAN A REUSED ONE.
    //
    // Every other scope answers "which part of the build sees this". An
    // advisory is seen by nobody in the build: it changes no compile line, no
    // link line and no source set, and `apply` therefore does not read its
    // slot. Spelling it `PackagePrivate` would have been the cheap move and
    // would have said something false — that it reaches this package's own
    // translation units.
    //
    // `RerunKey` is the closest existing value (also "not a build input"), and
    // it is still wrong: a re-run key feeds a MACHINE decision, an advisory
    // feeds a person.
    Advisory,
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
    LinkerScript,   // "-T <absolute path>" — freestanding link layout
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

inline constexpr std::array<Def, 20> kTable{{
    //  wire                    tag                  slot                    scope                  transform                must   missingPrefix                 missingSuffix                                    since
    {"cxxflag",             "cxxflag",           Slot::CxxFlags,         Scope::PackagePrivate, Transform::Verbatim,      false, "",                           "",                                              1},
    {"cflag",               "cflag",             Slot::CFlags,           Scope::PackagePrivate, Transform::Verbatim,      false, "",                           "",                                              1},
    {"link-lib",            "ldflag",            Slot::LdFlags,          Scope::LinkGlobal,     Transform::LibFlag,       false, "",                           "",                                              1},
    {"link-search",         "ldflag",            Slot::LdFlags,          Scope::LinkGlobal,     Transform::LibSearchPath, false, "",                           "",                                              1},
    {"cfg",                 "define",            Slot::Defines,          Scope::PackagePrivate, Transform::DefinePrefix,  false, "",                           "",                                              1},
    {"generated",           "generated",         Slot::Generated,        Scope::SourceSet,      Transform::Verbatim,      true,  "declared generated source",  "but it does not exist after the run",           1},
    {"source",              "source",            Slot::Sources,          Scope::SourceSet,      Transform::Verbatim,      true,  "selected source",            "(mcpp:source=) but no such file exists",         1},
    // ⚠️ LinkGlobal, and that is the whole reason this row exists.
    //
    // A board-support package is the one thing that knows a board's memory
    // layout, and a linker script is how that layout is expressed. Every other
    // way of getting one onto the link line is package-private (`cxxflag`) or
    // cannot express the flag at all (`link-lib` emits `-l`, `link-search`
    // emits `-L`), so before this row a BSP could supply the C library and the
    // startup code and still not supply the layout — leaving the one thing a
    // user cannot write for themselves as the one thing they had to.
    //
    // The supply-chain rule the PackagePrivate rows enforce is not weakened:
    // this widens the LINK, which `link-lib`/`link-search` already do, and not
    // the public compile interface.
    //
    // Single-valued in practice — two scripts on one line is an lld error, and
    // that error names both, which is a better diagnostic than anything a
    // conflict check here would produce.
    // ⚠️ `mustExistAfterRun` is FALSE, and not by oversight. That contract
    // assumes the directive's value IS a path (`generated=`, `source=`), and
    // this one's transformed value is `-T <path>` — so the check would test
    // the wrong string and reject a script that is right there. Special-casing
    // the contract for one row would cost more than it buys: lld's own error
    // is already exact ("cannot find linker script <path>"), which is the
    // condition the contract exists to make legible.
    // ⚠️ One argv TOKEN per line, in emission order.
    //
    // argv is an ordered list and a directive is one line = one value, so the
    // list is built by repetition. The alternative — a JSON array, as `action`
    // uses — would introduce an escaping contract for a payload that never
    // nests, and `action` pays that cost only because it has six fields.
    //
    // Verbatim: a runner token is not a path to normalize (it may be `-bios`),
    // and the producer already resolved the emulator absolutely, because a
    // bare name resolves through PATH to a shim that dispatches against its
    // OWNER home — measured in CI as `xlings: '…' is not installed` from a job
    // where the same name had answered `--version` two steps earlier.
    {"runner",              "runner",            Slot::Runner,           Scope::RunGlobal,      Transform::Verbatim,      false, "",                           "",                                              4},
    {"flash",               "flash",             Slot::Flash,            Scope::RunGlobal,      Transform::Verbatim,      false, "",                           "",                                              6},
    {"monitor",             "monitor",           Slot::Monitor,          Scope::RunGlobal,      Transform::Verbatim,      false, "",                           "",                                              6},
    {"debug",               "debug",             Slot::Debug,            Scope::RunGlobal,      Transform::Verbatim,      false, "",                           "",                                              6},
    {"runner-exclusive",    "runner-exclusive",  Slot::RunnerExclusive,  Scope::RunGlobal,      Transform::Verbatim,      false, "",                           "",                                              6},
    {"link-script",         "ldflag",            Slot::LdFlags,          Scope::LinkGlobal,     Transform::LinkerScript,  false, "",                           "",                                              3},
    {"include-dir",         "include-dir",       Slot::IncludeDirs,      Scope::PackagePrivate, Transform::AbsPath,       false, "",                           "",                                              1},
    {"include-dir-after",   "include-dir-after", Slot::IncludeDirsAfter, Scope::PackagePrivate, Transform::AbsPath,       false, "",                           "",                                              1},
    {"rerun-if-changed",    "",                  Slot::RerunFiles,       Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              1},
    {"rerun-if-env-changed","",                  Slot::RerunEnv,         Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              1},
    {"rerun-if-changed-glob","",                 Slot::RerunGlobs,       Scope::RerunKey,       Transform::Verbatim,      false, "",                           "",                                              2},
    // ⚠️ THE ONE THING A BUILD PROGRAM COULD NOT DO BEFORE: SUCCEED AND STILL
    // SAY SOMETHING.
    //
    // mcpp captures a build program and prints what it captured only on a
    // NON-ZERO exit. So a program that finished its job but found something
    // the user needs to know had no channel: a `std::cerr` note printed
    // nothing on exactly the builds that needed it, which is worse than no
    // note at all because it looks like a fix. Failing instead is not an
    // option either — the build succeeded.
    //
    // The motivating case: a manifest's `[xlings] deps` is a DECLARATION, not
    // an install trigger, so on a machine that has not installed the emulator
    // `xpkg_dir` returns empty and the program configures no runner. Correct,
    // silent, and indistinguishable to the user from a package that forgot.
    //
    // ⚠️ `tag` IS NON-EMPTY, AND THAT IS LOAD-BEARING. A build program's
    // result is cached and a hit does not re-run it, so an advisory that lived
    // only on the run path would appear once and never again — the same
    // failure shape as the note that was deleted, arrived at from the other
    // side. A non-empty tag puts it in the cache record, and `serialize` /
    // `accept_cache_record` are table-driven, so the replay costs nothing.
    //
    // ⚠️ kCacheEpoch is deliberately NOT bumped for this row. Entries written
    // before it carry no `d warning` line, and the programs that wrote them
    // could not emit one — so replaying them yields exactly what the program
    // said, and the entry is still correct. Bumping would re-run every build
    // program in every project on upgrade and buy nothing. The reverse
    // direction is already safe: an older engine reading a newer entry hits
    // the unknown-tag path and discards the whole record.
    {"warning",             "warning",           Slot::Warnings,         Scope::Advisory,       Transform::Verbatim,      false, "", "", 5},
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

// ── Advisories ─────────────────────────────────────────────────────────────
//
// The `mcpp:warning=` lines a program emitted, each already prefixed with the
// package it came from.
//
// ⚠️ THE FORMATTING LIVES HERE AND THE PRINTING DOES NOT, for two reasons that
// pull the same way. This module deliberately imports no UI — a directive
// table that knew how to draw would be a different kind of thing. And there
// are TWO call sites, a run and a cache hit, which is exactly the shape that
// drifts: one source for the wording means they cannot disagree about what
// they say, and a test covers whether they both say it.
//
// The package name comes from the caller because a build program cannot spell
// it reliably — in a workspace it would have to know which member it is.
std::vector<std::string> advisories(std::string_view packageName, const Directives& d);

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
//
// ⚠️ ONLY FOR OUTPUTS THAT ARE TRANSLATION UNITS, which is why this needs the
// table. A placeholder exists so the SCAN has something to read, and the scan
// never reads a header — but writing one anyway turned "the generator did not
// run" into "the header is empty", and mcpp#534 was diagnosed as a race for
// exactly that reason: the file was on disk, so the action looked like it had
// run. A missing file is the honest report, and after the ordering fix the
// generator runs before anything reads it either way.
void prepare_actions(std::vector<mcpp::manifest::BuildAction>& actions,
                     const std::filesystem::path& pkgRoot,
                     const mcpp::ExtensionTable& extensions);

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
bool is_compilable_output(const std::filesystem::path& p,
                          const mcpp::ExtensionTable& t);

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
    // Native spelling (see mcpp::modgraph::native_path_from_generic): a
    // directive path like `generated/modules/x` would otherwise stay mixed
    // on MSVC and leak into include flags / the CDB.
    fs::path pp = mcpp::modgraph::native_path_from_generic(p);
    if (pp.is_relative()) pp = base / pp;
    return pp.lexically_normal().string();
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
        // Absolute on purpose: the link runs in the build directory, so a
        // relative script path resolves against the wrong root and lld
        // answers "cannot find linker script link.ld" — measured.
        case Transform::LinkerScript:  return "-T " + abs_against(root, raw);
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
        // ⚠️ NOT "so it must be a typo".
        //
        // That is what this said, and adding `link-script` in protocol 3
        // proved it wrong: a package written against a newer mcpp reaches an
        // older one with the OLDER engine's protocol number stamped on it —
        // the announcement is substituted at build.mcpp compile time by
        // whichever engine is running, not carried by the package. So the two
        // numbers agreeing says nothing about whether the KEY is from the
        // future, and an old mcpp cannot tell the two cases apart. Naming both
        // is the only honest thing it can do, and the upgrade is the cheaper
        // one to try first.
        return std::format(
            "build.mcpp emitted directive(s) this mcpp does not know: {}.\n"
            "       Either the package was written for a newer mcpp (try "
            "`mcpp self update`),\n"
            "       or the directive is misspelled. This mcpp speaks protocol "
            "{}; the protocol number\n"
            "       cannot distinguish the two, because it is stamped by "
            "whichever mcpp compiled\n"
            "       the program, not by the package.",
            list, kProtocolVersion);
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

std::vector<std::string> advisories(std::string_view packageName, const Directives& d) {
    std::vector<std::string> out;
    for (auto const& w : d.at(Slot::Warnings)) {
        // Unnamed rather than "<unnamed package>": a workspace member always
        // has a name, and an unnamed root is a single-package build where the
        // prefix would be noise.
        out.push_back(packageName.empty()
                          ? std::string(w)
                          : std::format("{}: {}", packageName, w));
    }
    return out;
}

void apply(mcpp::manifest::Manifest& m, const Directives& d) {
    auto& bc = m.buildConfig;
    auto const& cxx      = d.at(Slot::CxxFlags);
    auto const& c        = d.at(Slot::CFlags);
    auto const& ld       = d.at(Slot::LdFlags);
    auto const& runner   = d.at(Slot::Runner);
    auto const& flash    = d.at(Slot::Flash);
    auto const& monitor  = d.at(Slot::Monitor);
    auto const& debugTpl = d.at(Slot::Debug);
    auto const& defines  = d.at(Slot::Defines);

    bc.cxxflags.insert(bc.cxxflags.end(), cxx.begin(), cxx.end());
    bc.cflags.insert(bc.cflags.end(), c.begin(), c.end());
    bc.ldflags.insert(bc.ldflags.end(), ld.begin(), ld.end());
    // Appended in emission order — the tokens ARE the argv.
    bc.runner.insert(bc.runner.end(), runner.begin(), runner.end());
    bc.flash.insert(bc.flash.end(), flash.begin(), flash.end());
    bc.monitor.insert(bc.monitor.end(), monitor.begin(), monitor.end());
    bc.debugger.insert(bc.debugger.end(), debugTpl.begin(), debugTpl.end());
    // ⚠️ ANY non-empty value sets it, and there is deliberately no way to unset
    // it from a second package. Exclusivity is a claim about the DEVICE: if one
    // package in the graph knows the target is a mutex, it is one, and a later
    // package saying nothing must not relax that.
    if (!d.at(Slot::RunnerExclusive).empty()) bc.runnerExclusive = true;
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
               : role == "object"   ? mcpp::manifest::BuildAction::Role::Object
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
        arr("targets", a.targets);
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

// Can a build program's declared output be fed to the compiler?
//
// This used to carry the fifth hand-written extension list — and the ONLY one
// that mentioned `.ixx`, which is how a generated `.ixx` was accepted here and
// then mis-handled by every stage after it. It now asks the classifier with
// the OWNING PACKAGE's table, so "mcpp will compile this" and "mcpp knows what
// this is" are the same question again.
bool is_compilable_output(const fs::path& p, const mcpp::ExtensionTable& t) {
    auto kind = mcpp::classify(p, t);
    return kind != mcpp::SourceKind::Header && kind != mcpp::SourceKind::Other;
}

void prepare_actions(std::vector<mcpp::manifest::BuildAction>& actions,
                     const fs::path& pkgRoot,
                     const mcpp::ExtensionTable& extensions) {
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
            // A placeholder exists so the scan has a translation unit to read.
            // A header is not one — nothing scans it, and the empty file it
            // used to leave behind is what made a generator that never ran
            // look like one that had (mcpp#534).
            if (!is_compilable_output(o, extensions)) continue;
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
