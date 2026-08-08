// The envelope is a contract, so it is tested like one.
//
// The lesson these assertions encode is xlings': `interface --list` declares
// 20 capabilities whose `outputSchema` is, for all 20, only
// `{"exitCode": integer}`. A schema that says nothing while looking like a
// contract is worse than none — a client reads `schemaVersion` and assumes
// there is one. So the shape is pinned here, and changing a field name has to
// turn this file red.
//
// Design: .agents/docs/2026-08-08-machine-readable-output-protocol-design.md

#include <gtest/gtest.h>

import std;
import mcpp.wire;
import mcpp.libs.json;

namespace w = mcpp::wire;

namespace {

// A client's ONLY reliable rule against an mcpp that predates the protocol is
// to parse stdout and require these two. `--protocol-version` cannot serve as
// the entry point: on older versions it is itself an unknown option. So the
// envelope must be self-identifying, and these two keys are what identifies
// it.
TEST(WireEnvelope, IsSelfIdentifying) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    ASSERT_TRUE(j.contains("schemaVersion")) << "a client detects by parsing";
    ASSERT_TRUE(j.contains("kind"));
    EXPECT_EQ(j["schemaVersion"], w::kEnvelopeVersion);
    EXPECT_EQ(j["kind"], "mcpp.env");
}

TEST(WireEnvelope, CarriesTheKindsOwnVersionSeparately) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.xpkg"});
    // Separate from schemaVersion on purpose: one global number would make a
    // field added to mcpp.env move the version a client reads for mcpp.xpkg,
    // with no way to tell which actually changed.
    ASSERT_TRUE(j.contains("kindVersion"));
    EXPECT_EQ(j["kindVersion"], 1);
}

TEST(WireEnvelope, AnUnknownKindHasNoVersion) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.not-a-kind"});
    EXPECT_EQ(j["kindVersion"], 0) << "silently claiming version 1 would be a lie";
}

// Effects are named, not boolean. `init-mcpp-home` is something a client may
// ignore; `exec-build-script` runs code out of the workspace and is the whole
// reason an untrusted-workspace gate exists. One bool cannot separate them.
TEST(WireEnvelope, EffectsAreNamed) {
    auto j = w::to_json(w::Envelope{
        .kind = "mcpp.env",
        .effects = {w::Effect::InitMcppHome, w::Effect::ExecBuildScript}});
    ASSERT_EQ(j["effects"].size(), 2u);
    EXPECT_EQ(j["effects"][0], "init-mcpp-home");
    EXPECT_EQ(j["effects"][1], "exec-build-script");
}

TEST(WireEnvelope, NoEffectsIsAnEmptyArrayNotAbsent) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.xpkg"});
    ASSERT_TRUE(j.contains("effects"));
    EXPECT_TRUE(j["effects"].is_array());
    EXPECT_TRUE(j["effects"].empty()) << "absent would mean 'unknown', not 'none'";
}

TEST(WireEnvelope, DiagnosticsCarryCodeAndPosition) {
    w::Diagnostic d{
        .code = "MCPP_MANIFEST_UNKNOWN_KEY",
        .severity = w::Severity::Warning,
        .message = "unknown key 'standrad'",
        .path = "mcpp.toml",
        .range = w::Range{{3, 1}, {3, 9}},
    };
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env", .diagnostics = {d}});
    ASSERT_EQ(j["diagnostics"].size(), 1u);
    auto const& g = j["diagnostics"][0];
    EXPECT_EQ(g["code"], "MCPP_MANIFEST_UNKNOWN_KEY");
    EXPECT_EQ(g["severity"], "warning");
    EXPECT_EQ(g["source"], "mcpp");
    EXPECT_EQ(g["path"], "mcpp.toml");
    EXPECT_EQ(g["range"]["start"]["line"], 3);
    EXPECT_EQ(g["range"]["end"]["column"], 9);
}

// A diagnostic without a location must not invent one: absent keys, not
// zeros. `line: 0` would send a client to a position that does not exist.
TEST(WireEnvelope, ADiagnosticWithoutALocationOmitsIt) {
    auto j = w::to_json(w::Envelope{
        .kind = "mcpp.env",
        .diagnostics = {{.code = "MCPP_X", .message = "no place"}}});
    auto const& g = j["diagnostics"][0];
    EXPECT_FALSE(g.contains("path"));
    EXPECT_FALSE(g.contains("range"));
}

TEST(WireEnvelope, ReportsMcppVersionAndProtocolWindow) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    EXPECT_FALSE(j["mcpp"]["version"].get<std::string>().empty());
    EXPECT_EQ(j["mcpp"]["protocol"]["min"], w::kEnvelopeVersion);
    EXPECT_EQ(j["mcpp"]["protocol"]["max"], w::kEnvelopeVersion);
}

// ── Negotiation ────────────────────────────────────────────────────────────

TEST(WireProtocol, ListsKindsAndTheirVersions) {
    auto j = w::protocol_document({
        {"self env",   {w::Effect::InitMcppHome}},
        {"xpkg parse", {}},
    });
    ASSERT_TRUE(j.contains("kinds"));
    // `{min,max}` alone cannot answer "do you speak mcpp.env, at what
    // version" — which is the question a client actually has.
    EXPECT_EQ(j["kinds"]["mcpp.env"], 1);
    EXPECT_EQ(j["kinds"]["mcpp.xpkg"], 1);
}

// The gate has to decide BEFORE running. By the time an envelope arrives,
// whatever it describes has already happened — so the effects have to be
// available statically too.
TEST(WireProtocol, DeclaresEffectsPerCommandStatically) {
    auto j = w::protocol_document({
        {"self env",   {w::Effect::InitMcppHome}},
        {"xpkg parse", {}},
    });
    ASSERT_TRUE(j["commands"].contains("self env"));
    EXPECT_EQ(j["commands"]["self env"]["effects"][0], "init-mcpp-home");
    EXPECT_TRUE(j["commands"]["xpkg parse"]["effects"].empty())
        << "measured on a fresh MCPP_HOME: it creates nothing";
}

TEST(WireProtocol, IsItselfSelfIdentifying) {
    auto j = w::protocol_document({});
    EXPECT_EQ(j["kind"], "mcpp.protocol");
    EXPECT_TRUE(j.contains("schemaVersion"));
}

// ── Format selection ───────────────────────────────────────────────────────

TEST(WireFormat, KnowsJsonAndRefusesTheRest) {
    EXPECT_EQ(w::parse_format("json"), w::Format::Json);
    EXPECT_FALSE(w::parse_format("yaml").has_value());
    EXPECT_FALSE(w::parse_format("ndjson").has_value()) << "reserved, not supported";
    EXPECT_FALSE(w::parse_format("").has_value());
}

TEST(WireFormat, TheRefusalNamesWhatIsSupported) {
    auto m = w::unsupported_format("yaml");
    EXPECT_NE(m.find("yaml"), std::string::npos);
    EXPECT_NE(m.find("json"), std::string::npos) << "tell the client what to ask for";
}

// LegacyJson is a distinct format, not a synonym. `--json` has shipped with a
// bare payload (`cache list --json` is `{root, entries}` at the top level, and
// this repo's e2e asserts those keys); enveloping it would break consumers.
TEST(WireFormat, LegacyJsonIsNotTheSameFormatAsJson) {
    EXPECT_NE(w::Format::LegacyJson, w::Format::Json);
}

// ── Golden shapes, one per kind ────────────────────────────────────────────
//
// docs/11-machine-output.md promises that within a kindVersion fields are
// added and never removed or renamed. A promise nobody can break is the thing
// this whole module was written against — `xlings interface --list` declares
// 20 capabilities whose outputSchema is, for all 20, only
// `{"exitCode": integer}`. So the promise is enforced here: these lists are
// the published key set, and removing or renaming one turns this file red.
//
// Adding a field does NOT turn it red, deliberately — that is the one change
// the contract allows.
namespace {

void expect_has_keys(const nlohmann::json& obj,
                     std::initializer_list<const char*> keys,
                     std::string_view what) {
    for (auto k : keys)
        EXPECT_TRUE(obj.contains(k))
            << what << " lost published key '" << k
            << "'. Removing or renaming one is a breaking change: bump the "
               "kind's version in mcpp.wire and say so in "
               "docs/11-machine-output.md.";
}

}  // namespace

TEST(WireGolden, EnvelopeKeySet) {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect_has_keys(j, {"schemaVersion", "kind", "kindVersion", "effects",
                        "mcpp", "data", "diagnostics"}, "the envelope");
    expect_has_keys(j["mcpp"], {"version", "protocol"}, "envelope.mcpp");
    expect_has_keys(j["mcpp"]["protocol"], {"min", "max"}, "envelope.mcpp.protocol");
}

TEST(WireGolden, DiagnosticKeySet) {
    auto j = w::to_json(w::Envelope{
        .kind = "mcpp.env",
        .diagnostics = {{.code = "C", .message = "m", .path = "p",
                         .range = w::Range{{1, 1}, {1, 2}}}}});
    expect_has_keys(j["diagnostics"][0],
                    {"code", "severity", "source", "message", "path", "range"},
                    "a diagnostic");
    expect_has_keys(j["diagnostics"][0]["range"], {"start", "end"}, "a range");
    expect_has_keys(j["diagnostics"][0]["range"]["start"], {"line", "column"},
                    "a position");
}

TEST(WireGolden, ProtocolDocumentKeySet) {
    auto j = w::protocol_document({{"self env", {w::Effect::InitMcppHome}}});
    expect_has_keys(j, {"schemaVersion", "kind", "mcpp", "envelope", "kinds",
                        "commands"}, "the protocol document");
    expect_has_keys(j["envelope"], {"min", "max"}, "protocol.envelope");
    expect_has_keys(j["commands"]["self env"], {"effects"}, "a command entry");
}

// Every effect name is part of the contract: a client matches on these
// strings, so renaming one silently changes what a gate lets through.
TEST(WireGolden, EffectNamesAreStable) {
    struct { w::Effect e; const char* name; } const expected[]{
        {w::Effect::InitMcppHome,     "init-mcpp-home"},
        {w::Effect::ReadProject,      "read-project"},
        {w::Effect::WriteProject,     "write-project"},
        {w::Effect::WriteGlobalCache, "write-global-cache"},
        {w::Effect::Network,          "network"},
        {w::Effect::ExecBuildScript,  "exec-build-script"},
    };
    for (auto const& x : expected)
        EXPECT_EQ(w::effect_name(x.e), x.name)
            << "effect names are matched by clients; renaming one changes "
               "what an untrusted-workspace gate admits";
}

TEST(WireGolden, SeverityNamesAreStable) {
    EXPECT_EQ(w::severity_name(w::Severity::Error),   "error");
    EXPECT_EQ(w::severity_name(w::Severity::Warning), "warning");
    EXPECT_EQ(w::severity_name(w::Severity::Note),    "note");
}

// The kinds this build claims to speak. A client reads this list to decide
// whether to bother calling; dropping one silently is a breaking change.
TEST(WireGolden, DeclaredKinds) {
    auto j = w::protocol_document({});
    expect_has_keys(j["kinds"], {"mcpp.env", "mcpp.xpkg", "mcpp.cache"},
                    "the kind list");
}

}  // namespace
