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

}  // namespace
