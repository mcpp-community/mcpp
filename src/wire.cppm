// mcpp.wire — the machine-readable output contract, in one place.
//
// Every JSON that leaves mcpp for a program rather than a person goes through
// here. That is the whole point: `xlings interface` declares 20 capabilities
// whose `outputSchema` is, for all 20, only `{"exitCode": integer}` — a schema
// that says nothing while looking like a contract. A client reads
// `schemaVersion` and assumes there is one. So this module owns the envelope
// AND the rules that keep it honest.
//
// Design: .agents/docs/2026-08-08-machine-readable-output-protocol-design.md
//
// ── Two decisions worth knowing before reading the code ────────────────────
//
// 1. THE ENVELOPE IS SELF-IDENTIFYING, and clients are told to detect it by
//    PARSING, never by exit code.
//
//    `mcpp --protocol-version` cannot solve the bootstrap problem it looks
//    like it solves: on every mcpp that predates it, that command IS the
//    "spawn something that might fail" case, and its failure is
//    indistinguishable from success by channel. Measured, before the fix in
//    this same series: it printed `Error: unknown option` to STDOUT with rc=1
//    and an empty stderr. Changing the spelling to `--json` changes nothing —
//    both hit the same unknown-option path.
//
//    So the only rule that works against versions that predate the protocol
//    is positive detection: read stdout, try to parse it, and require
//    `schemaVersion` + `kind` to be there. `--protocol-version` remains
//    useful on versions that have it (one fewer spawn), but it is an
//    optimisation, not the foundation.
//
// 2. EFFECTS, NOT A `destructive` BOOLEAN.
//
//    A boolean cannot separate "creates mcpp's own home on first run" from
//    "executes code out of the workspace", and an IDE's untrusted-workspace
//    gate cares only about the second. Measured on a fresh MCPP_HOME:
//    `xpkg parse --json` and `cache list --json` create NOTHING, while
//    `self env` creates six entries — so the field does carry information,
//    but only if the effect is NAMED. `init-mcpp-home` is something a client
//    can reasonably ignore; `exec-build-script` is not.
//
//    Effects are also reported in `--protocol-version` as a static table,
//    because a gate has to decide BEFORE running — by the time an envelope
//    arrives, whatever it describes has already happened.
module;

#include <cstdio>

export module mcpp.wire;

import std;
import mcpp.version;
import mcpp.libs.json;

export namespace mcpp::wire {

// The envelope's own version. Bumped only when the envelope SHAPE changes —
// never for a change inside one command's `data`, which carries its own
// version in `kinds` below. A single global number would couple unrelated
// kinds: a field added to `mcpp.env` would push the version a client reads
// for `mcpp.xpkg`, and the client could not tell which one actually moved.
inline constexpr int kEnvelopeVersion = 1;

// Each kind's `data` version, and the list of kinds this build knows.
//
// Reported by `--protocol-version` so a client can ask "do you speak
// mcpp.env, and at what version" without spawning the command and guessing
// from what comes back.
struct KindVersion { std::string_view kind; int version; };

inline constexpr std::array<KindVersion, 3> kKinds{{
    {"mcpp.env",   1},
    {"mcpp.xpkg",  1},
    {"mcpp.cache", 1},
}};

// What running a command does, beyond writing to stdout.
//
// Named rather than boolean. The scope is deliberately part of the name:
// `write-project` and `init-mcpp-home` are both "writes files", and only one
// of them is a reason for an IDE to refuse.
enum class Effect {
    InitMcppHome,     // creates $MCPP_HOME on first use. Outside the project.
    ReadProject,      // reads the manifest / sources
    WriteProject,     // writes into the project tree (target/, CDB, …)
    WriteGlobalCache, // writes the shared build cache
    Network,          // may fetch
    ExecBuildScript,  // runs code FROM THE WORKSPACE (build.mcpp). The one an
                      // untrusted-workspace gate exists for.
};

constexpr std::string_view effect_name(Effect e) {
    switch (e) {
        case Effect::InitMcppHome:     return "init-mcpp-home";
        case Effect::ReadProject:      return "read-project";
        case Effect::WriteProject:     return "write-project";
        case Effect::WriteGlobalCache: return "write-global-cache";
        case Effect::Network:          return "network";
        case Effect::ExecBuildScript:  return "exec-build-script";
    }
    return "unknown";
}

// ── Diagnostics ────────────────────────────────────────────────────────────
//
// A position is 1-based, matching every compiler and every editor. `column`
// counts UTF-8 bytes, not code points: it is what a client needs to index the
// same file mcpp read.
struct Position { int line = 0; int column = 0; };
struct Range    { Position start; Position end; };

enum class Severity { Error, Warning, Note };

constexpr std::string_view severity_name(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
    }
    return "note";
}

// `code` is required, and deliberately so. `MCPP_IDE_CONFIGURE_FAILED` --
// one code covering every failure, with the docs forbidding clients from
// parsing the human message -- is structured in name only. A new failure
// branch either gets its own code, or is declared indivisible on purpose.
struct Diagnostic {
    std::string code;
    Severity    severity = Severity::Error;
    std::string message;
    std::string path;                    // optional
    std::optional<Range> range;          // optional
};

// ── The envelope ───────────────────────────────────────────────────────────

struct Envelope {
    std::string_view kind;
    std::vector<Effect> effects;
    nlohmann::json data = nlohmann::json::object();
    std::vector<Diagnostic> diagnostics;
};

inline int kind_version(std::string_view kind) {
    for (auto const& k : kKinds) if (k.kind == kind) return k.version;
    return 0;
}

inline nlohmann::json to_json(const Diagnostic& d) {
    nlohmann::json j{
        {"code",     d.code},
        {"severity", std::string(severity_name(d.severity))},
        {"source",   "mcpp"},
        {"message",  d.message},
    };
    if (!d.path.empty()) j["path"] = d.path;
    if (d.range) {
        j["range"] = {
            {"start", {{"line", d.range->start.line}, {"column", d.range->start.column}}},
            {"end",   {{"line", d.range->end.line},   {"column", d.range->end.column}}},
        };
    }
    return j;
}

inline nlohmann::json to_json(const Envelope& e) {
    nlohmann::json effects = nlohmann::json::array();
    for (auto f : e.effects) effects.push_back(std::string(effect_name(f)));

    nlohmann::json diags = nlohmann::json::array();
    for (auto const& d : e.diagnostics) diags.push_back(to_json(d));

    return nlohmann::json{
        {"schemaVersion", kEnvelopeVersion},
        {"kind",          std::string(e.kind)},
        {"kindVersion",   kind_version(e.kind)},
        {"effects",       std::move(effects)},
        {"mcpp", {
            {"version", std::string(mcpp::MCPP_VERSION)},
            {"protocol", {{"min", kEnvelopeVersion}, {"max", kEnvelopeVersion}}},
        }},
        {"data",        e.data},
        {"diagnostics", std::move(diags)},
    };
}

// Serialise and write to stdout. Two spaces, trailing newline: a client reads
// it with a JSON parser either way, and a person reading a bug report should
// not have to reformat it first.
inline void emit(const Envelope& e) {
    std::println("{}", to_json(e).dump(2));
}

// ── Negotiation ────────────────────────────────────────────────────────────
//
// `mcpp --protocol-version`. Answers three things a client would otherwise
// have to discover by spawning commands and interpreting failures: which
// envelope versions, which kinds at which versions, and what each command
// does before it prints anything.
// The caller supplies the table. This module deliberately does NOT know which
// commands exist: the effects of `self env` are a fact about `self env`, and
// belong next to where it is declared. Keeping the list here would mean adding
// a command in one file and remembering to describe it in another.
struct CommandEffects { std::string_view command; std::vector<Effect> effects; };

inline nlohmann::json protocol_document(
        const std::vector<CommandEffects>& commands) {
    nlohmann::json kinds = nlohmann::json::object();
    for (auto const& k : kKinds) kinds[std::string(k.kind)] = k.version;

    nlohmann::json cmds = nlohmann::json::object();
    for (auto const& c : commands) {
        nlohmann::json fx = nlohmann::json::array();
        for (auto f : c.effects) fx.push_back(std::string(effect_name(f)));
        cmds[std::string(c.command)] = nlohmann::json{{"effects", std::move(fx)}};
    }

    return nlohmann::json{
        {"schemaVersion", kEnvelopeVersion},
        {"kind",          "mcpp.protocol"},
        {"mcpp",          {{"version", std::string(mcpp::MCPP_VERSION)}}},
        {"envelope",      {{"min", kEnvelopeVersion}, {"max", kEnvelopeVersion}}},
        {"kinds",         std::move(kinds)},
        {"commands",      std::move(cmds)},
    };
}

// ── Output format selection ────────────────────────────────────────────────
//
// One spelling in the core (`--format <value>`), with `--json` accepted for
// ever at the entry points. Same shape as `toolchain/compat.cppm`: the only
// place that knows the old spelling is the boundary, and no deprecation
// warning is ever printed — clients already parse this output, and a warning
// would land in the middle of it.
//
// IMPORTANT, and the reason `--json` is not merely an alias: the two are not
// the same OUTPUT. `cache list --json` has shipped with `{root, entries}` at
// the top level and this repository's own e2e asserts those keys. Wrapping
// that in an envelope is a breaking change. So `--json` keeps the legacy
// payload for ever, and `--format json` is the enveloped one. Spelling
// compatibility is not payload compatibility, and treating them as the same
// thing is how a published contract gets broken quietly.
enum class Format { Human, LegacyJson, Json };

// Unknown values are NOT decided here — the caller reports them, because only
// the caller knows the command name for the message. See `unsupported_format`.
inline std::optional<Format> parse_format(std::string_view v) {
    if (v == "json") return Format::Json;
    return std::nullopt;
}

// The message for an unsupported `--format`. Goes to stderr with exit 2 (the
// caller does that): a request that does not yet know what it will be given
// must not write into the channel the protocol owns, and a client that finds
// no JSON on stdout has its answer.
inline std::string unsupported_format(std::string_view got) {
    return std::format("unsupported --format '{}'; expected: json", got);
}

}  // namespace mcpp::wire
