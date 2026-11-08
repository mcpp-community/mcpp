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


import std;
import mcpplibs.mtest;
import mcpp.wire;
import nlohmann.json;

using namespace mcpplibs::mtest;

namespace w = mcpp::wire;

namespace {

// A client's ONLY reliable rule against an mcpp that predates the protocol is
// to parse stdout and require these two. `--protocol-version` cannot serve as
// the entry point: on older versions it is itself an unknown option. So the
// envelope must be self-identifying, and these two keys are what identifies
// it.
UTest is_self_identifying{"WireEnvelope/IsSelfIdentifying", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect(fatal(j.contains("schemaVersion"))) << "a client detects by parsing";
    expect(fatal(j.contains("kind")));
    expect(eq(j["schemaVersion"], w::kEnvelopeVersion));
    expect(eq(j["kind"], "mcpp.env"));
}};

UTest carries_the_kinds_own_version_separately{"WireEnvelope/CarriesTheKindsOwnVersionSeparately", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.xpkg"});
    // Separate from schemaVersion on purpose: one global number would make a
    // field added to mcpp.env move the version a client reads for mcpp.xpkg,
    // with no way to tell which actually changed.
    expect(fatal(j.contains("kindVersion")));
    expect(eq(j["kindVersion"], 1));
}};

UTest an_unknown_kind_has_no_version{"WireEnvelope/AnUnknownKindHasNoVersion", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.not-a-kind"});
    expect(eq(j["kindVersion"], 0)) << "silently claiming version 1 would be a lie";
}};

// Effects are named, not boolean. `init-mcpp-home` is something a client may
// ignore; `exec-build-script` runs code out of the workspace and is the whole
// reason an untrusted-workspace gate exists. One bool cannot separate them.
UTest effects_are_named{"WireEnvelope/EffectsAreNamed", [] {
    auto j = w::to_json(w::Envelope{
        .kind = "mcpp.env",
        .effects = {w::Effect::InitMcppHome, w::Effect::ExecBuildScript}});
    expect(fatal(eq(j["effects"].size(), 2u)));
    expect(eq(j["effects"][0], "init-mcpp-home"));
    expect(eq(j["effects"][1], "exec-build-script"));
}};

UTest no_effects_is_an_empty_array_not_absent{"WireEnvelope/NoEffectsIsAnEmptyArrayNotAbsent", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.xpkg"});
    expect(fatal(j.contains("effects")));
    expect(j["effects"].is_array());
    expect(j["effects"].empty()) << "absent would mean 'unknown', not 'none'";
}};

UTest diagnostics_carry_code_and_position{"WireEnvelope/DiagnosticsCarryCodeAndPosition", [] {
    w::Diagnostic d{
        .code = "MCPP_MANIFEST_UNKNOWN_KEY",
        .severity = w::Severity::Warning,
        .message = "unknown key 'standrad'",
        .path = "mcpp.toml",
        .range = w::Range{{3, 1}, {3, 9}},
    };
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env", .diagnostics = {d}});
    expect(fatal(eq(j["diagnostics"].size(), 1u)));
    auto const& g = j["diagnostics"][0];
    expect(eq(g["code"], "MCPP_MANIFEST_UNKNOWN_KEY"));
    expect(eq(g["severity"], "warning"));
    expect(eq(g["source"], "mcpp"));
    expect(eq(g["path"], "mcpp.toml"));
    expect(eq(g["range"]["start"]["line"], 3));
    expect(eq(g["range"]["end"]["column"], 9));
}};

// A diagnostic without a location must not invent one: absent keys, not
// zeros. `line: 0` would send a client to a position that does not exist.
UTest a_diagnostic_without_a_location_omits_it{"WireEnvelope/ADiagnosticWithoutALocationOmitsIt", [] {
    auto j = w::to_json(w::Envelope{
        .kind = "mcpp.env",
        .diagnostics = {{.code = "MCPP_X", .message = "no place"}}});
    auto const& g = j["diagnostics"][0];
    expect(!g.contains("path"));
    expect(!g.contains("range"));
}};

UTest reports_mcpp_version_and_protocol_window{"WireEnvelope/ReportsMcppVersionAndProtocolWindow", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect(!(j["mcpp"]["version"].get<std::string>().empty()));
    expect(eq(j["mcpp"]["protocol"]["min"], w::kEnvelopeVersion));
    expect(eq(j["mcpp"]["protocol"]["max"], w::kEnvelopeVersion));
}};

// Every envelope carries the features beside the version, so a client's one
// `self env` (or any job's envelope) says what the project's mcpp can do
// (design 2026-10-30 part 2 §4.3); the list is the protocol document's.
UTest carries_the_features_of_the_protocol_document{"WireEnvelope/CarriesTheFeaturesOfTheProtocolDocument", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect(fatal(j["mcpp"].contains("features") && j["mcpp"]["features"].is_array()));
    expect(eq(j["mcpp"]["features"], w::protocol_document({})["features"]));
}};

// ── Negotiation ────────────────────────────────────────────────────────────

UTest lists_kinds_and_their_versions{"WireProtocol/ListsKindsAndTheirVersions", [] {
    auto j = w::protocol_document({
        {"self env",   {w::Effect::InitMcppHome}},
        {"xpkg parse", {}},
    });
    expect(fatal(j.contains("kinds")));
    // `{min,max}` alone cannot answer "do you speak mcpp.env, at what
    // version" — which is the question a client actually has.
    expect(eq(j["kinds"]["mcpp.env"], 1));
    expect(eq(j["kinds"]["mcpp.xpkg"], 1));
}};

// The gate has to decide BEFORE running. By the time an envelope arrives,
// whatever it describes has already happened — so the effects have to be
// available statically too.
UTest declares_effects_per_command_statically{"WireProtocol/DeclaresEffectsPerCommandStatically", [] {
    auto j = w::protocol_document({
        {"self env",   {w::Effect::InitMcppHome}},
        {"xpkg parse", {}},
    });
    expect(fatal(j["commands"].contains("self env")));
    expect(eq(j["commands"]["self env"]["effects"][0], "init-mcpp-home"));
    expect(j["commands"]["xpkg parse"]["effects"].empty())
        << "measured on a fresh MCPP_HOME: it creates nothing";
}};

// A client gates on a feature, not on a version number (design 2026-10-30
// part 2 §4.3): the list is present, and the names are the stable ones.
UTest names_the_features_a_client_gates_on{"WireProtocol/NamesTheFeaturesAClientGatesOn", [] {
    auto j = w::protocol_document({});
    expect(fatal(j.contains("features") && j["features"].is_array()));
    std::vector<std::string> names;
    for (auto const& f : j["features"]) names.push_back(f.get<std::string>());
    expect(ne(std::ranges::find(names, "target-sets"), names.end()));
    expect(ne(std::ranges::find(names, "pkg-format-list"), names.end()));
    expect(ne(std::ranges::find(names, "test-launch"), names.end()));
    expect(ne(std::ranges::find(names, "launch-stream"), names.end()));
    expect(ne(std::ranges::find(names, "test-protocol"), names.end()));
}};

UTest is_itself_self_identifying{"WireProtocol/IsItselfSelfIdentifying", [] {
    auto j = w::protocol_document({});
    expect(eq(j["kind"], "mcpp.protocol"));
    expect(j.contains("schemaVersion"));
}};

// ── Format selection ───────────────────────────────────────────────────────

UTest knows_json_and_refuses_the_rest{"WireFormat/KnowsJsonAndRefusesTheRest", [] {
    expect(eq(w::parse_format("json"), w::Format::Json));
    expect(!(w::parse_format("yaml").has_value()));
    expect(!(w::parse_format("ndjson").has_value())) << "reserved, not supported";
    expect(!(w::parse_format("").has_value()));
}};

UTest the_refusal_names_what_is_supported{"WireFormat/TheRefusalNamesWhatIsSupported", [] {
    auto m = w::unsupported_format("yaml");
    expect(ne(m.find("yaml"), std::string::npos));
    expect(ne(m.find("json"), std::string::npos)) << "tell the client what to ask for";
}};

// LegacyJson is a distinct format, not a synonym. `--json` has shipped with a
// bare payload (`cache list --json` is `{root, entries}` at the top level, and
// this repo's e2e asserts those keys); enveloping it would break consumers.
UTest legacy_json_is_not_the_same_format_as_json{"WireFormat/LegacyJsonIsNotTheSameFormatAsJson", [] {
    expect(ne(w::Format::LegacyJson, w::Format::Json));
}};

// ── Golden shapes, one per kind ────────────────────────────────────────────
//
// docs/50-machine-output.md promises that within a kindVersion fields are
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
        expect(obj.contains(k))
            << what << " lost published key '" << k
            << "'. Removing or renaming one is a breaking change: bump the "
               "kind's version in mcpp.wire and say so in "
               "docs/50-machine-output.md.";
}

}  // namespace

UTest envelope_key_set{"WireGolden/EnvelopeKeySet", [] {
    auto j = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect_has_keys(j, {"schemaVersion", "kind", "kindVersion", "effects",
                        "mcpp", "data", "diagnostics"}, "the envelope");
    expect_has_keys(j["mcpp"], {"version", "protocol", "features"}, "envelope.mcpp");
    expect_has_keys(j["mcpp"]["protocol"], {"min", "max"}, "envelope.mcpp.protocol");
}};

UTest diagnostic_key_set{"WireGolden/DiagnosticKeySet", [] {
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
}};

UTest protocol_document_key_set{"WireGolden/ProtocolDocumentKeySet", [] {
    auto j = w::protocol_document({{"self env", {w::Effect::InitMcppHome}}});
    expect_has_keys(j, {"schemaVersion", "kind", "mcpp", "envelope", "kinds",
                        "commands", "features"}, "the protocol document");
    expect_has_keys(j["envelope"], {"min", "max"}, "protocol.envelope");
    expect_has_keys(j["commands"]["self env"], {"effects"}, "a command entry");
}};

// Every effect name is part of the contract: a client matches on these
// strings, so renaming one silently changes what a gate lets through.
UTest effect_names_are_stable{"WireGolden/EffectNamesAreStable", [] {
    struct { w::Effect e; const char* name; } const expected[]{
        {w::Effect::InitMcppHome,     "init-mcpp-home"},
        {w::Effect::ReadProject,      "read-project"},
        {w::Effect::WriteProject,     "write-project"},
        {w::Effect::WriteGlobalCache, "write-global-cache"},
        {w::Effect::Network,          "network"},
        {w::Effect::ExecBuildScript,  "exec-build-script"},
    };
    for (auto const& x : expected)
        expect(eq(w::effect_name(x.e), x.name))
            << "effect names are matched by clients; renaming one changes "
               "what an untrusted-workspace gate admits";
}};

UTest severity_names_are_stable{"WireGolden/SeverityNamesAreStable", [] {
    expect(eq(w::severity_name(w::Severity::Error), "error"));
    expect(eq(w::severity_name(w::Severity::Warning), "warning"));
    expect(eq(w::severity_name(w::Severity::Note), "note"));
}};

// The kinds this build claims to speak. A client reads this list to decide
// whether to bother calling; dropping one silently is a breaking change.
UTest declared_kinds{"WireGolden/DeclaredKinds", [] {
    auto j = w::protocol_document({});
    expect_has_keys(j["kinds"], {"mcpp.env", "mcpp.xpkg", "mcpp.cache",
                                 "mcpp.build-database"},
                    "the kind list");
    expect(eq(j["kinds"]["mcpp.build-database"], 1));
}};

// A failure that leaves nothing to describe omits `data`: a consumer of the
// build database reads "no data" as "the command failed" (S2 0.2.0 section 3.4),
// so an empty object in its place would read as a success with nothing in it.
UTest null_data_is_omitted{"WireGolden/NullDataIsOmitted", [] {
    auto failed = w::to_json(w::Envelope{
        .kind = "mcpp.build-database",
        .effects = {w::Effect::ReadProject},
        .data = nullptr,
        .diagnostics = {{"MCPP_BUILD_DATABASE_PLAN_FAILED", w::Severity::Error, "x"}},
    });
    expect(!failed.contains("data")) << failed.dump();
    expect(failed.contains("diagnostics"));
    auto ok = w::to_json(w::Envelope{.kind = "mcpp.env"});
    expect(ok.contains("data")) << ok.dump();
}};

}  // namespace
