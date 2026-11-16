// Reading the `subos_info` block xlings writes into a subos's .xlings.json.
//
// This is a CROSS-REPO CONTRACT, so the assertions are about the wire format
// and about what happens when it is absent or newer than we understand —
// not about internal behaviour. The degradation cases carry the weight: a
// subos made before xlings grew the block is the common case on any machine
// that has been around a while, and silence there is what made mcpp#352
// expensive to find in the first place.


import std;
import mcpplibs.mtest;
import mcpp.runtime.launch_env;
import mcpp.provider.xlings.subos_info;

using namespace mcpplibs::mtest;

namespace su = mcpp::provider::xlings::subos;

namespace {

struct Tmp {
    std::filesystem::path dir;
    Tmp() {
        dir = std::filesystem::temp_directory_path()
            / std::format("mcpp_subos_test_{}", std::random_device{}());
        std::filesystem::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    void write(std::string_view body) const {
        std::ofstream(dir / ".xlings.json") << body;
    }
};

UTest reads_runtime_and_env_declarations{"SubosInfo/ReadsRuntimeAndEnvDeclarations", [] {
    Tmp t;
    t.write(R"({
      "workspace": {},
      "subos_info": {
        "schema_version": 1,
        "runtime": "glibc@2.39",
        "host_glibc": "2.43",
        "envs": {
          "mesa@25.0.7.1": [
            { "var": "LIBGL_DRIVERS_PATH", "op": "prepend",
              "value": "${subosdir}/usr/lib/dri" },
            { "var": "XDG_DATA_DIRS", "op": "prepend",
              "value": "${subosdir}/share" }
          ]
        },
        "runtime_contract": {
          "providers": [
            { "capability": "render.demo",
              "provider": { "namespace": "xim", "name": "renderer",
                "version": "4.0.0", "source": "xim-pkgindex@rev" } }
          ],
          "artifacts": [
            { "role": "driver",
              "provider": { "namespace": "xim", "name": "renderer",
                "version": "4.0.0", "source": "xim-pkgindex@rev" },
              "path": "${subosdir}/runtime/renderer.bin",
              "provenance": "subos_view", "abi": "fixture-v1",
              "digest": "sha256:def", "host_fingerprint": "host-2" }
          ]
        }
      }
    })");
    auto info = su::read(t.dir);
    expect(bool(info.present));
    expect(eq(info.schema, 1));
    expect(eq(info.runtime, "glibc@2.39"));
    expect(eq(info.hostGlibc, "2.43"));
    expect(fatal(eq(info.providers.size(), 1u)));
    expect(eq(info.providers[0].binding, "mesa@25.0.7.1"));
    expect(fatal(eq(info.providers[0].decls.size(), 2u)));
    expect(eq(info.providers[0].decls[0].var, "LIBGL_DRIVERS_PATH"));
    expect(fatal(eq(info.runtimeProviders.size(), 1u)));
    expect(eq(info.runtimeProviders[0].capability, "render.demo"));
    expect(eq(info.runtimeProviders[0].provider.namespace_, "xim"));
    expect(eq(info.runtimeProviders[0].provider.name, "renderer"));
    expect(fatal(eq(info.runtimeArtifacts.size(), 1u)));
    expect(eq(info.runtimeArtifacts[0].path, (t.dir / "runtime/renderer.bin").lexically_normal()));
    expect(eq(info.runtimeArtifacts[0].hostFingerprint, "host-2"));
    expect(info.note.empty());
}};

UTest orders_generic_contract_by_the_complete_fact_identity{"SubosInfo/OrdersGenericContractByTheCompleteFactIdentity", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
      "runtime_contract":{
        "providers":[
          {"capability":"render.demo","provider":{"namespace":"xim",
            "name":"renderer","version":"1","source":"z-index"}},
          {"capability":"render.demo","provider":{"namespace":"xim",
            "name":"renderer","version":"1","source":"a-index"}}
        ],
        "artifacts":[
          {"role":"driver","provider":{"namespace":"xim","name":"renderer",
            "version":"1","source":"same"},"path":"runtime/driver",
            "provenance":"z-view","digest":"sha256:z"},
          {"role":"driver","provider":{"namespace":"xim","name":"renderer",
            "version":"1","source":"same"},"path":"runtime/driver",
            "provenance":"a-view","digest":"sha256:a"}
        ]
      }}})");
    auto info = su::read(t.dir);
    expect(fatal(eq(info.runtimeProviders.size(), 2u)));
    expect(eq(info.runtimeProviders[0].provider.source, "a-index"));
    expect(eq(info.runtimeProviders[1].provider.source, "z-index"));
    expect(fatal(eq(info.runtimeArtifacts.size(), 2u)));
    expect(eq(info.runtimeArtifacts[0].provenance, "a-view"));
    expect(eq(info.runtimeArtifacts[1].provenance, "z-view"));
}};

// `${subosdir}` expands against the subos the block was read FROM, not
// against any global notion of "the" subos. That is what lets a program
// built under one subos run correctly under another.
UTest resolves_subosdir_placeholder{"SubosInfo/ResolvesSubosdirPlaceholder", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
      "envs":{"mesa@1":[
        {"var":"LIBGL_DRIVERS_PATH","op":"prepend","value":"${subosdir}/usr/lib/dri"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    expect(fatal(eq(env.size(), 1u)));
    expect(eq(env[0].first, "LIBGL_DRIVERS_PATH"));
    // Literal concatenation, NOT a path join. The separator in the declaration
    // belongs to the subos manifest and is substituted verbatim; turning it
    // into the host's would rewrite a value we do not own. On Windows the two
    // spellings differ and the path-join form is the wrong expectation.
    expect(eq(env[0].second, t.dir.string() + "/usr/lib/dri"));
}};

// Several providers may contribute to one variable — that is the normal
// shape for the graphics stack, where mesa and a vendor bridge both add an
// EGL vendor directory. `prepend` joins them; it must not drop either.
UTest prepend_joins_providers_in_order{"SubosInfo/PrependJoinsProvidersInOrder", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a-mesa@1":[{"var":"V","op":"prepend","value":"${subosdir}/one"}],
      "b-vendor@1":[{"var":"V","op":"prepend","value":"${subosdir}/two"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    expect(fatal(eq(env.size(), 1u)));
    const auto sep = mcpp::runtime::launch_env::path_list_separator();
    expect(eq(env[0].second, t.dir.string() + "/two" + sep + t.dir.string() + "/one"))
        << "both providers must survive, later binding front-most";
}};

// The same value arriving twice must not accumulate: nested invocations
// would otherwise grow the variable without bound.
UTest prepend_deduplicates{"SubosInfo/PrependDeduplicates", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"prepend","value":"${subosdir}/x"}],
      "b@1":[{"var":"V","op":"prepend","value":"${subosdir}/x"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    expect(fatal(eq(env.size(), 1u)));
    // One entry, not two. The de-duplication has to split on the PLATFORM's
    // list separator: keyed on ':' it would cut "C:\\x" apart on Windows,
    // match nothing, and grow the list on every nested invocation.
    expect(eq(env[0].second, t.dir.string() + "/x"));
    expect(eq(env[0].second.find(mcpp::runtime::launch_env::path_list_separator()), std::string::npos));
}};

// `set` supplies a value only when the caller did not define the variable.
// Presence — not non-emptiness — is the boundary, matching xlings: an
// explicitly empty export is still an intentional caller value.
UTest set_applies_when_ambient_is_absent{"SubosInfo/SetAppliesWhenAmbientIsAbsent", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"prepend","value":"/one"}],
      "b@1":[{"var":"V","op":"set","value":"/two"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    expect(fatal(eq(env.size(), 1u)));
    expect(eq(env[0].second, "/two"));
}};

UTest set_preserves_present_ambient_value{"SubosInfo/SetPreservesPresentAmbientValue", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"set","value":"/subos"}]}}})");
    auto env = su::resolve_env(
        su::read(t.dir), t.dir,
        [](std::string_view var) -> std::optional<std::string> {
            if (var == "V") return "/caller";
            return std::nullopt;
        });
    expect(fatal(eq(env.size(), 1u)));
    expect(eq(env[0].second, "/caller"));
}};

UTest set_preserves_explicitly_empty_ambient_value{"SubosInfo/SetPreservesExplicitlyEmptyAmbientValue", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"set","value":"/subos"}]}}})");
    auto env = su::resolve_env(
        su::read(t.dir), t.dir,
        [](std::string_view var) -> std::optional<std::string> {
            if (var == "V") return std::string{};
            return std::nullopt;
        });
    expect(fatal(eq(env.size(), 1u)));
    expect(env[0].second.empty());
}};

// A subos made before xlings grew the block. Degrade, and SAY SO — this is
// the common case on an existing machine, and mcpp's own sandbox subos was
// measured in exactly this state.
UTest missing_block_degrades_with_a_note{"SubosInfo/MissingBlockDegradesWithANote", [] {
    Tmp t;
    t.write(R"({"workspace":{}})");
    auto info = su::read(t.dir);
    expect(!info.present);
    expect(info.runtime.empty());
    expect(!info.note.empty());
    expect(su::resolve_env(info, t.dir).empty());
}};

// A schema newer than we understand: read what we can, and say we are behind.
// Refusing outright would make a newer xlings break an older mcpp, which is
// the failure mode the index-floor incident already paid for once.
UTest newer_schema_is_used_and_announced{"SubosInfo/NewerSchemaIsUsedAndAnnounced", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":99,"runtime":"glibc@2.44","envs":[]}})");
    auto info = su::read(t.dir);
    expect(bool(info.present));
    expect(eq(info.runtime, "glibc@2.44"));
    expect(!info.note.empty());
}};

UTest no_file_at_all_is_not_a_crash{"SubosInfo/NoFileAtAllIsNotACrash", [] {
    Tmp t;   // nothing written
    auto info = su::read(t.dir);
    expect(!info.present);
    expect(!info.note.empty());
}};

UTest malformed_json_degrades{"SubosInfo/MalformedJsonDegrades", [] {
    Tmp t;
    t.write("{ this is not json");
    auto info = su::read(t.dir);
    expect(!info.present);
    expect(!info.note.empty());
}};

// The family mapping is part of the cross-repo contract: xlings derives it
// from the runtime string too, and the two must agree or a subos and a build
// will disagree about what ABI they are talking about.
UTest family_of_mirrors_xlings{"SubosInfo/FamilyOfMirrorsXlings", [] {
    expect(eq(su::family_of("glibc@2.39"), "linux-x86_64-glibc"));
    expect(eq(su::family_of("musl@1.2.5"), "linux-x86_64-musl"));
    expect(eq(su::family_of("glibc@2.39", "aarch64"), "linux-aarch64-glibc"));
    expect(eq(su::family_of("wasi-libc@1"), "wasm32-wasi"));
    expect(eq(su::family_of("macos_sdk@14.0", "arm64"), "darwin-arm64"));
    expect(eq(su::family_of("ucrt@10"), "windows-x86_64-ucrt"));
    expect(eq(su::family_of("nonsense@1"), "unknown"));
    // No '@' at all is not a binding; it must not be read as one.
    expect(eq(su::family_of("glibc"), "linux-x86_64-glibc"));
}};


// A VERBATIM capture of what a real xlings wrote, after `xlings install
// graphics` on an NVIDIA host. Reformatted for width and nothing else -- keys,
// nesting and spelling are as found on disk.
//
// This test exists because its absence shipped a broken feature. The first
// version of this file hand-wrote every fixture in a shape the reader also
// expected and xlings never produces: `envs` as an array of {binding, decls}.
// Ten tests passed against a format that does not exist, and against a real
// subos the released build applied no variables at all -- silently, because
// "no providers" and "nothing declared" look identical.
//
// A fixture composed from the same understanding as the parser cannot catch
// that. Only one taken from the writer can.
UTest real_xlings_capture{"SubosInfo/RealXlingsCapture", [] {
    Tmp t;
    t.write(R"({
      "subos_info": {
        "created_at": "2026-08-08T01:40:00Z",
        "created_by": "xlings 2026.8.7.1",
        "runtime": "glibc@2.39",
        "schema_version": 1,
        "envs": {
          "mesa@25.0.7.1": [
            {"op": "prepend", "value": "${subosdir}/usr/lib/dri", "var": "LIBGL_DRIVERS_PATH"},
            {"op": "prepend", "value": "${subosdir}/share/glvnd/egl_vendor.d", "var": "__EGL_VENDOR_LIBRARY_DIRS"},
            {"op": "prepend", "value": "${subosdir}/share", "var": "XDG_DATA_DIRS"}
          ],
          "nvidia-gl-host-link@0.1.1": [
            {"op": "prepend", "value": "${subosdir}/share/glvnd/egl_vendor.d", "var": "__EGL_VENDOR_LIBRARY_DIRS"}
          ]
        }
      },
      "workspace": {}
    })");

    auto info = su::read(t.dir);
    expect(fatal(bool(info.present)));
    expect(eq(info.runtime, "glibc@2.39"));
    expect(fatal(eq(info.providers.size(), 2u)));
    expect(eq(info.providers[0].binding, "mesa@25.0.7.1"));
    expect(eq(info.providers[1].binding, "nvidia-gl-host-link@0.1.1"));

    auto env = su::resolve_env(info, t.dir);
    expect(fatal(eq(env.size(), 3u))) << "all three graphics variables must be produced";

    std::map<std::string, std::string> byVar;
    for (auto& [k, v] : env) byVar[k] = v;
    const auto sep = mcpp::runtime::launch_env::path_list_separator();
    expect(eq(byVar["LIBGL_DRIVERS_PATH"], t.dir.string() + "/usr/lib/dri"));
    expect(eq(byVar["XDG_DATA_DIRS"], t.dir.string() + "/share"));
    // Both providers name the same vendor directory; de-duplication must
    // leave exactly one, or libglvnd sees it twice and enumerates the device
    // twice -- which is a defect xlings hit on its own side.
    expect(eq(byVar["__EGL_VENDOR_LIBRARY_DIRS"], t.dir.string() + "/share/glvnd/egl_vendor.d"));
    expect(eq(byVar["__EGL_VENDOR_LIBRARY_DIRS"].find(sep), std::string::npos));
}};

// xlings drops a declaration whose op it does not recognise. A reader more
// permissive than its writer eventually applies something the writer meant to
// reject, so this asserts the same refusal rather than a tolerant guess.
UTest unknown_op_is_dropped_like_xlings_drops{"SubosInfo/UnknownOpIsDroppedLikeXlingsDrops", [] {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"append","value":"/nope"},
             {"var":"W","op":"prepend","value":"/yes"},
             {"var":"","op":"prepend","value":"/no-name"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    expect(fatal(eq(env.size(), 1u)));
    expect(eq(env[0].first, "W"));
    expect(eq(env[0].second, "/yes"));
}};




// `prepend` prepends TO the caller's value rather than replacing it. Same
// reason: the pair replaces the variable, so emitting the declared value alone
// discards whatever search path the user had.
UTest prepend_keeps_the_exported_value{"SubosResolveEnv/PrependKeepsTheExportedValue", [] {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"LD_LIBRARY_PATH","op":"prepend","value":"/sub/lib"}]}}})");
    auto info = su::read(t.dir);
    auto out = su::resolve_env(
        info, t.dir, [](std::string_view v) -> std::optional<std::string> {
            if (v == "LD_LIBRARY_PATH") return std::string("/user/lib");
            return std::nullopt;
        });
    expect(fatal(eq(out.size(), 1u)));
    const auto sep = mcpp::runtime::launch_env::path_list_separator();
    expect(eq(out[0].second, std::string("/sub/lib") + sep + "/user/lib"));
}};

// Already there: prepending again would grow the list on every nested run.
UTest prepend_is_idempotent_against_the_exported_value{"SubosResolveEnv/PrependIsIdempotentAgainstTheExportedValue", [] {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"LD_LIBRARY_PATH","op":"prepend","value":"/sub/lib"}]}}})");
    auto info = su::read(t.dir);
    const auto sep = mcpp::runtime::launch_env::path_list_separator();
    const auto existing = std::string("/sub/lib") + sep + "/user/lib";
    auto out = su::resolve_env(
        info, t.dir, [&](std::string_view v) -> std::optional<std::string> {
            if (v == "LD_LIBRARY_PATH") return existing;
            return std::nullopt;
        });
    expect(fatal(eq(out.size(), 1u)));
    expect(eq(out[0].second, existing));
}};

// xlings defines `set` as set-if-absent. mcpp must use the same presence
// boundary or the exact same SubOS changes behaviour depending on which tool
// launches the program.
UTest set_yields_to_an_exported_value{"SubosResolveEnv/SetYieldsToAnExportedValue", [] {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,
        "runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"GALLIUM_DRIVER","op":"set","value":"d3d12"}]}}})");
    auto info = su::read(t.dir);
    auto out = su::resolve_env(
        info, t.dir, [](std::string_view v) -> std::optional<std::string> {
            if (v == "GALLIUM_DRIVER") return std::string("llvmpipe");
            return std::nullopt;
        });
    expect(fatal(eq(out.size(), 1u)));
    expect(eq(out[0].second, "llvmpipe"));
}};

}  // namespace
