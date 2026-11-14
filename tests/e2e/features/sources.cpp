// Feature-gated sources: a dependency's under `mcpp test`, a manifest's [features] `sources`, a gate
// and a provider, per-glob feature flags (formerly tests/e2e/100, 106, 138, 146).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The most recently written <name> under `target`, outside the build programs' trees.
std::optional<fs::path> newest_graph(const fs::path& target, std::string_view name) {
    std::optional<fs::path> best;
    for (const auto& f : s::find_files(target, std::regex_replace(std::string(name), std::regex{R"(\.)"}, R"(\.)")))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos && (!best || fs::last_write_time(f) > fs::last_write_time(*best))) best = f;
    return best;
}

E2E sources{"sources", [] {
    "100 feature sources test mode"_test = [] {
        // A dependency's feature-gated `sources` are compiled under `mcpp test` as under
        // `mcpp build`, and not without the feature.
        auto app = s::project::scaffold("app");
        app.remove("src/main.cpp");
        app.remove("tests/test_smoke.cpp");
        app.write("mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.compat]\ncjson = { version = \"1.7.19\", features = [\"utils\"] }\n");
        app.write("tests/t.cpp", "#include <cJSON.h>\n#include <cJSON_Utils.h>\nint main() {\n    cJSON* root = cJSON_Parse(\"{\\\"a\\\":1}\");\n    if (!root) return 1;\n    char* p = cJSONUtils_FindPointerFromObjectTo(root, root);\n    int ok = (p != 0);\n    cJSON_Delete(root);\n    return ok ? 0 : 1;\n}\n");
        expect(fatal(e2e::succeeded(app.mcpp({"test"})))) << "feature-gated sources not compiled under `mcpp test`";
        auto nj = newest_graph(app.at("target"), "test.ninja");
        expect(fatal(nj && e2e::contains(s::read_file(*nj), "cJSON_Utils"))) << "cJSON_Utils.c absent from the test-mode build graph";
        app.write("src/main.cpp", "#include <cJSON.h>\n#include <cJSON_Utils.h>\nint main() {\n    cJSON* root = cJSON_Parse(\"{\\\"a\\\":1}\");\n    char* p = cJSONUtils_FindPointerFromObjectTo(root, root);\n    (void)p; cJSON_Delete(root); return 0;\n}\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build with feature sources failed";
        nj = newest_graph(app.at("target"), "build.ninja");
        expect(fatal(nj && e2e::contains(s::read_file(*nj), "cJSON_Utils"))) << "cJSON_Utils.c absent from build graph";
        app.write("mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.compat]\ncjson = \"1.7.19\"\n");
        app.write("src/main.cpp", "#include <cJSON.h>\nint main() { cJSON* r = cJSON_Parse(\"{\\\"a\\\":1}\"); cJSON_Delete(r); return 0; }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "default (no feature) build failed";
        nj = newest_graph(app.at("target"), "build.ninja");
        expect(fatal(nj.has_value())) << "no build.ninja";
        expect(s::lacks(s::read_file(*nj), "cJSON_Utils")) << "cJSON_Utils.c compiled without the `utils` feature";
    };

    "106 feature gated sources toml"_test[s::gcc] = [] {
        // [features] `sources`: off and on, under build and under test.
        auto p = s::project::scaffold("featsrc");
        p.write("src/extra.cpp", "extern \"C\" int extra_answer() { return 42; }\n");
        p.write("src/main.cpp", "import std;\nextern \"C\" int extra_answer();\nint main() {\n#ifdef HAS_EXTRA\n    std::println(\"extra = {}\", extra_answer());\n#else\n    std::println(\"extra = none\");\n#endif\n    return 0;\n}\n");
        p.write("mcpp.toml", "[package]\nname    = \"featsrc\"\nversion = \"0.1.0\"\n\n[features]\ndefault = []\nextra   = { sources = [\"src/extra.cpp\"], defines = [\"HAS_EXTRA\"] }\n");
        p.remove("tests/test_smoke.cpp");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build (off) failed";
        expect(fatal(s::find_files(p.at("target"), R"(extra\.o)").empty())) << "extra.cpp compiled with feature off";
        expect(fatal(eq(s::last_line(p.mcpp({"run"}).out), std::string{"extra = none"}))) << "unexpected (off)";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--features", "extra"})))) << "build (on) failed";
        const auto bin = s::newest_file(p.at("target"), "featsrc");
        expect(fatal(bin.has_value())) << "binary not found";
        expect(fatal(eq(s::last_line(s::run(*bin).out), std::string{"extra = 42"}))) << "unexpected (on)";
        p.write("tests/test_featsrc.cpp", "#ifdef HAS_EXTRA\nextern \"C\" int extra_answer();\nint main() { return extra_answer() == 42 ? 0 : 1; }\n#else\nint main() { return 0; }\n#endif\n");
        expect(fatal(e2e::succeeded(p.mcpp({"test"})))) << "mcpp test (off) failed";
        expect(e2e::succeeded(p.mcpp({"test", "--features", "extra"}))) << "mcpp test (on) failed";
    };
    "138 feature sources gate vs provider"_test[s::unix_shell] = [] {
        // A feature that gates a source (in no base list, but under the inferred `src/**`) and a
        // feature that switches a source the package provides, under build and test.
        const auto tmp = s::case_dir();
        s::project provider{tmp / "provider", {{"mcpp.toml", R"cpp([package]
name    = "provider"
version = "0.1.0"

# NO `[build] sources`, WHICH IS THE POINT. The base set is the inferred
# `src/**`, which matches `src/gated/only_with_feature.cpp` — so removing the
# feature's glob STRING from a list it was never in gates nothing.
[features]
default = []
gated   = { sources = ["src/gated/**"] }
)cpp"}, {"src/lib.cpp", R"cpp(int provider_value() { return 7; }
)cpp"}, {"src/gated/only_with_feature.cpp", R"cpp(// The header arrives with the feature's dependencies and does not exist
// without it. Compiling this file without the feature is the defect.
#include <a_header_the_feature_would_have_brought.h>
int gated_value() { return 1; }
)cpp"}, {"tests/basic.cpp", R"cpp(extern int provider_value();
int main() { return provider_value() == 7 ? 0 : 1; }
)cpp"}}};
        expect(fatal(e2e::succeeded(provider.mcpp({"build"})))) << "provider: build compiled a feature-only source";
        expect(fatal(e2e::succeeded(provider.mcpp({"test"})))) << "provider: TEST compiled a feature-only source — the defect this file exists for";
        expect(fatal(s::find_files(provider.at("target"), ".*only_with_feature.*").empty())) << "provider: an object was produced for a source the feature did not activate";
        auto gated = provider.mcpp({"build", "--features", "gated"});
        expect(fatal(failed(gated))) << "provider: the feature did not bring its own source in";
        expect(fatal(e2e::contains(s::text(gated), "a_header_the_feature_would_have_brought.h"))) << "provider: activating the feature failed for some other reason";
        s::project gate{tmp / "gate", {{"mcpp.toml", R"cpp([package]
name    = "gate"
version = "0.1.0"

# `src/gated_main.cpp` IS IN BOTH LISTS, EXACTLY AS gtest'S DESCRIPTOR HAS IT.
# The package provides the file; the feature is a switch over it.
[build]
sources = ["src/lib.cpp", "src/gated_main.cpp"]

[features]
default = []
main    = { sources = ["src/gated_main.cpp"] }
)cpp"}, {"src/lib.cpp", R"cpp(int gate_value() { return 11; }
)cpp"}, {"src/gated_main.cpp", R"cpp(int gated_main_marker() { return 42; }
)cpp"}, {"tests/basic.cpp", R"cpp(extern int gate_value();
int main() { return gate_value() == 11 ? 0 : 1; }
)cpp"}}};
        expect(fatal(e2e::succeeded(gate.mcpp({"build"})))) << "gate: build failed";
        expect(fatal(s::find_files(gate.at("target"), ".*gated_main.*").empty())) << "gate: build compiled a gated source with the feature inactive";
        gate.remove("target");
        expect(fatal(e2e::succeeded(gate.mcpp({"test"})))) << "gate: test failed";
        expect(!s::find_files(gate.at("target"), ".*gated_main.*").empty()) << "gate: a source the package provides unconditionally vanished under `mcpp test`";
    };

    "146 feature flags"_test[s::gcc] = [] {
        // [features].<name>.flags: per-feature per-glob flags reach exactly the matched TU, off
        // and on, under build and test; a dead glob warns only when its feature is on; a
        // dependency's feature flags stay private while its defines propagate.
        auto p = s::project::scaffold("featflags");
        p.write("src/simd/kernel.cpp", R"cpp(#ifndef KERNEL_TAG
#error "feature flags did not reach the glob-matched TU"
#endif
extern "C" int kernel_tag() { return KERNEL_TAG; }
)cpp");
        p.write("src/plain.cpp", R"cpp(#ifdef KERNEL_TAG
#error "feature flags leaked outside their glob (must be per-TU, not package-wide)"
#endif
extern "C" int plain_ok() { return 1; }
)cpp");
        p.write("src/main.cpp", R"cpp(import std;
extern "C" int plain_ok();
#ifdef MCPP_FEATURE_SIMD
extern "C" int kernel_tag();
#endif
int main() {
#ifdef MCPP_FEATURE_SIMD
    std::println("tag = {}", kernel_tag() + plain_ok());
#else
    std::println("tag = none");
#endif
    return 0;
}
)cpp");
        p.write("mcpp.toml", R"cpp([package]
name    = "featflags"
version = "0.1.0"

[build]
sources = ["src/*.cpp"]

[features]
default = []
simd    = { sources = ["src/simd/**"], flags = [
              { glob = "src/simd/**", defines = ["KERNEL_TAG=6"] } ] }
dead    = { flags = [ { glob = "src/never/**", defines = ["NOPE"] } ] }
)cpp");
        p.remove("tests/test_smoke.cpp");
        auto off = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(off))) << "build (off) failed";
        expect(fatal(s::lacks(s::text(off), "matched no source file"))) << "feature-off build must not warn about feature flag globs";
        expect(fatal(eq(s::last_line(p.mcpp({"run"}).out), std::string{"tag = none"}))) << "unexpected (off)";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--features", "simd"})))) << "build (on) failed";
        const auto bin = s::newest_file(p.at("target"), "featflags");
        expect(fatal(bin.has_value())) << "binary not found";
        expect(fatal(eq(s::last_line(s::run(*bin).out), std::string{"tag = 7"}))) << "unexpected (on) (want 6+1)";
        auto dead = p.mcpp({"build", "--features", "simd,dead"});
        expect(fatal(e2e::succeeded(dead))) << "build (dead) failed";
        expect(fatal(e2e::contains(s::text(dead), "features.dead.flags glob 'src/never/**' matched no source file"))) << "missing feature-scoped dead-glob warning";
        p.write("tests/test_featflags.cpp", R"cpp(#ifdef MCPP_FEATURE_SIMD
extern "C" int kernel_tag();
int main() { return kernel_tag() == 6 ? 0 : 1; }
#else
int main() { return 0; }
#endif
)cpp");
        auto test_off = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(test_off))) << "mcpp test (off) failed";
        expect(fatal(s::lacks(s::text(test_off), "matched no source file"))) << "feature-off test must not warn about feature flag globs";
        expect(fatal(e2e::succeeded(p.mcpp({"test", "--features", "simd"})))) << "mcpp test (on) failed";
        const auto tmp = s::case_dir();
        s::project{tmp / "widget", {{"mcpp.toml", R"cpp([package]
name    = "widget"
version = "0.1.0"

[features]
default = []
turbo   = { defines = ["WIDGET_IFACE=1"], flags = [
              { glob = "src/**", defines = ["WIDGET_PRIVATE=1"] } ] }

[targets.widget]
kind = "lib"
)cpp"}, {"src/widget.cppm", R"cpp(export module widget;
#ifndef WIDGET_PRIVATE
#error "feature flags did not reach the dep's own matched TU"
#endif
export int widget_anchor() { return WIDGET_PRIVATE; }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
widget = { path = "../widget", features = ["turbo"] }
)cpp"}, {"src/main.cpp", R"cpp(import widget;
#ifndef WIDGET_IFACE
#error "feature defines must interface-propagate to the consumer"
#endif
#ifdef WIDGET_PRIVATE
#error "feature flags must NOT propagate to the consumer"
#endif
int main() { return widget_anchor() == 1 ? 0 : 1; }
)cpp"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "dep build failed";
        expect(e2e::succeeded(app.mcpp({"run"}))) << "dep run failed";
    };
}};

}  // namespace
