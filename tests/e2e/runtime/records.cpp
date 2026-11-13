// What the build records of the runtime: requirements and providers kept apart, provider-neutral
// provenance that `why runtime` reads back without resolving, the symbol provision verdict of a
// legitimate and a conflicting arrangement, and post-link verdicts that survive an invocation and
// the other command (formerly tests/e2e/66, 207, 307, 323).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> resolutions(const fs::path& root) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root, R"(resolution\.json)"))
        if (!f.generic_string().contains("/.build-mcpp/")) out.push_back(f);
    return out;
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

// 307's verdict(): the field of the symbol-provision entry whose path ends with "app".
std::string verdict(const fs::path& res, std::string_view field) {
    const auto doc = s::json_file(res);
    if (!doc.is_object() || !doc.contains("runtime") || !doc["runtime"].contains("symbol_provision")) return "MISSING";
    const auto& entries = doc["runtime"]["symbol_provision"];
    if (entries.empty()) return "MISSING";
    for (const auto& e : entries)
        if (e.value("path", std::string()).ends_with("app")) {
            if (!e.contains(field)) return "MISSING";
            const auto& v = e[std::string(field)];
            return v.is_string() ? v.get<std::string>() : v.dump();
        }
    return "NO-APP-ENTRY";
}

// 323's paths_in: `sed -n '/"<name>"/,/^ *\]/p' <file> | grep -oE 'bin/[A-Za-z_0-9]+' | sort -u`.
std::string paths_in(const fs::path& file, std::string_view name) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return {};
    const auto quoted = std::format("\"{}\"", name);
    static const std::regex end{R"(^ *\])"};
    static const std::regex path{R"(bin/[A-Za-z_0-9]+)"};
    std::set<std::string> found;
    bool in = false;
    for (const auto& line : split(s::read_file(file), '\n')) {
        if (!in) {
            if (!line.contains(quoted)) continue;
            in = true;
        } else if (std::regex_search(line, end)) {
            in = false;
        }
        for (auto it = std::sregex_iterator(line.begin(), line.end(), path); it != std::sregex_iterator(); ++it) found.insert(it->str());
    }
    std::string out;
    for (const auto& p : found) out += p + " ";
    return out;
}

E2E records{"records", [] {
    "66 runtime provides"_test = [] {
        // Only an explicit `provides` owns a capability; requiring it never makes the requester
        // its provider, and an override cannot promote it.
        const auto tmp = s::case_dir();
        auto mk_dep = [&](std::string_view name, std::string_view runtime) {
            s::project(tmp / name, {{"mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n\n[runtime]\n{1}\n", name, runtime)},
                                    {"src/lib.cppm", std::format("export module {0};\nexport int {0}_id() {{ return 1; }}\n", name)}});
        };
        mk_dep("weakdep", "capabilities = [\"test.cap.demo\"]");
        mk_dep("strongdep", "capabilities = [\"test.cap.demo\"]\nprovides     = [\"test.cap.demo\"]");
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nweakdep   = { path = \"../weakdep\" }\nstrongdep = { path = \"../strongdep\" }\n"},
                                     {"src/main.cpp", "import std;\nimport weakdep;\nimport strongdep;\nint main() { std::println(\"{}\", weakdep_id() + strongdep_id()); return 0; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto res = resolutions(app.at("target"));
        expect(fatal(!res.empty())) << "no resolution.json produced";
        const auto plan = s::json_file(res.front());
        std::vector<nlohmann::json> providers;
        for (const auto& p : plan["runtime"]["providers"])
            if (p["capability"] == "test.cap.demo") providers.push_back(p);
        expect(fatal(providers.size() == 1u)) << nlohmann::json(providers).dump();
        expect(fatal(providers[0]["provider"]["canonical"] == "mcpplibs.strongdep@0.1.0")) << providers[0]["provider"].dump();
        std::size_t weak = 0;
        for (const auto& q : plan["runtime"]["requirements"])
            if (q["value"] == "test.cap.demo" && q["requester"]["canonical"] == "mcpplibs.weakdep@0.1.0") ++weak;
        expect(fatal(weak == 1u)) << plan["runtime"]["requirements"].dump();
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[runtime.\"test.cap.demo\"]\nprovider = \"weakdep\"\n");
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(!r)) << "weak requester was incorrectly promoted to provider: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "does not name a provider"))) << "missing exact provider diagnostic: " << s::text(r);
    };

    "207 runtime contract provenance"_test[s::elf][s::python3][s::unix_shell] = [] {
        // Two providers of one short name stay exact, a requirement cannot self-provide, and
        // `mcpp why runtime` reads the stored resolution: it succeeds with the manifest broken
        // and launches no graphics probe.
        const auto tmp = s::case_dir();
        auto mk_provider = [&](std::string_view ns, std::string_view value) {
            s::project(tmp / ns, {{"runtime/libbackend.fact", std::format("artifact-{}\n", ns)},
                                  {"mcpp.toml", std::format(R"([package]
namespace = "{0}"
name = "backend"
version = "{1}"

[modules]
sources = ["src/**/*.cppm"]

[targets.backend]
kind = "lib"

[runtime]
provides = ["render.demo"]
artifacts = [
  {{ role = "library", path = "runtime/libbackend.fact", provenance = "payload", abi = "fixture-v1", digest = "sha256:{0}" }},
]
)", ns, value)},
                                  {"src/backend.cppm", std::format("export module {0}.backend;\nexport int {0}_backend_value() {{ return {1}; }}\n", ns, value.substr(0, value.find('.')))}});
        };
        mk_provider("alpha", "2.0.0");
        mk_provider("beta", "3.0.0");
        s::project app(tmp / "app", {{"mcpp.toml", R"([package]
name = "app"
version = "1.0.0"

[dependencies.alpha]
backend = { path = "../alpha" }

[dependencies.beta]
backend = { path = "../beta" }

[runtime]
requirements = [
  { kind = "capability", value = "render.demo", phase = "run", required = true },
  { kind = "capability", value = "abi:musl", phase = "link", required = false },
]

[targets.app]
kind = "bin"
main = "src/main.cpp"
)"},
                                     {"src/main.cpp", "import alpha.backend;\nimport beta.backend;\nint main() { return alpha_backend_value() + beta_backend_value() == 5 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "runtime contract fixture build failed: " << s::text(r);
        const auto res = resolutions(app.at("target"));
        expect(fatal(!res.empty())) << "resolution.json missing";
        const auto doc = s::json_file(res.front());
        const auto& runtime = doc["runtime"];
        expect(fatal(doc["schema_version"] == 2)) << doc.dump();
        std::vector<std::string> ids;
        for (const auto& p : runtime["providers"])
            if (p["capability"] == "render.demo") {
                ids.push_back(p["provider"]["canonical"].get<std::string>());
                expect(fatal(p["provider"]["source"].get<std::string>().starts_with("path+"))) << p.dump();
            }
        expect(fatal(ids == std::vector<std::string>{"alpha.backend@2.0.0", "beta.backend@3.0.0"})) << runtime["providers"].dump();
        std::vector<nlohmann::json> requirements, optional;
        for (const auto& q : runtime["requirements"]) {
            if (q["value"] == "render.demo") requirements.push_back(q);
            if (q["value"] == "abi:musl") optional.push_back(q);
        }
        expect(fatal(requirements.size() == 1u)) << runtime["requirements"].dump();
        const auto requester = requirements[0]["requester"]["canonical"].get<std::string>();
        expect(fatal(requester == "mcpplibs.app@1.0.0")) << requirements[0].dump();
        expect(fatal(!std::ranges::contains(ids, requester))) << requirements[0].dump();
        expect(fatal(optional.size() == 1u && optional[0]["required"] == false)) << nlohmann::json(optional).dump();
        std::vector<std::string> artifact_ids;
        for (const auto& a : runtime["artifacts"]) {
            artifact_ids.push_back(a["provider"]["canonical"].get<std::string>());
            expect(fatal(a["provenance"] == "payload")) << runtime["artifacts"].dump();
        }
        expect(fatal(artifact_ids == ids)) << runtime["artifacts"].dump();
        expect(fatal(runtime["search"]["format"] == "elf")) << runtime["search"].dump();
        expect(fatal(runtime["validation"]["status"] == "pass")) << runtime["validation"].dump();
        const auto& hash = runtime["binding"]["contract_hash"];
        expect(fatal(!hash.is_null() && hash != "" && hash != false)) << runtime["binding"].dump();
        // The explanation reads stored facts: the manifest no longer parses, and familiar
        // graphics probes lead PATH.
        const auto good = app.read("mcpp.toml");
        app.write("mcpp.toml", "[package\n");
        const auto probes = tmp / "probes.log";
        for (std::string name : {"vulkaninfo", "glxinfo", "nvidia-smi"})
            s::write_executable(app.at("fake-bin/" + name), std::format("#!/usr/bin/env bash\nprintf '%s\\n' '{}' >> '{}'\nexit 99\n", name, probes.string()));
        r = app.mcpp({"why", "runtime"}, {.env = {{"PATH", app.at("fake-bin").string() + ":" + s::env("PATH").value_or("")}}});
        const auto why = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "why runtime re-resolved instead of reading stored facts: " << why;
        app.write("mcpp.toml", good);
        expect(fatal(!fs::exists(probes))) << "mcpp launched a provider-specific probe: " << (fs::exists(probes) ? s::read_file(probes) : "");
        for (std::string_view want : {"alpha.backend@2.0.0", "beta.backend@3.0.0", "validation: pass", "owned by xlings"})
            expect(fatal(e2e::contains(why, want))) << why;
    };

    "307 symbol provision"_test[s::elf][s::python3] = [] {
        // A shared library over a static package (linked into it) is silent and measured; a
        // second provider of a name the executable holds is reported with both providers, is
        // real at run time, and fails --strict.
        const auto python = s::on_path("python3");
        expect(fatal(python.has_value())) << "no python to load the library";
        const auto tmp = s::case_dir();
        s::project core(tmp / "core", {{"mcpp.toml", "[package]\nname    = \"core\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\nsources    = [\"src/*.c\"]\n[targets.core]\nkind = \"lib\"\n"},
                                       {"src/core.c", "int shared_answer(void) { return 7; }\n"}});
        s::project wrap(tmp / "wrap", {{"mcpp.toml", "[package]\nname    = \"wrap\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\nsources    = [\"src/*.c\"]\n[targets.wrap]\nkind = \"shared\"\n[dependencies]\ncore = { path = \"../core\" }\n"},
                                       {"src/wrap.c", "int shared_answer(void);\nint wrap_call(void) { return shared_answer() + 1; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n[dependencies]\nwrap = { path = \"../wrap\" }\n"},
                                     {"src/main.c", "#include <stdio.h>\nint wrap_call(void);\nint main(void) { printf(\"%d\\n\", wrap_call()); return 0; }\n"}});
        fs::create_directories(tmp / "alt/src");
        // half 1
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto res = resolutions(app.at("target"));
        expect(fatal(!res.empty())) << "FAIL: no resolution.json";
        const auto status = verdict(res.front(), "status");
        expect(fatal(status == "clean")) << "FAIL: the legitimate shared→static arrangement reported '" << status << "': " << s::read_file(res.front());
        const auto exported = verdict(res.front(), "exported");
        const auto total = verdict(res.front(), "dynamic_symbols");
        long long e = -1, t = -1;
        std::from_chars(exported.data(), exported.data() + exported.size(), e);
        std::from_chars(total.data(), total.data() + total.size(), t);
        expect(fatal(e >= 0 && t > e)) << "FAIL: implausible denominator (" << exported << " of " << total << ")";
        const auto wrapso = res.front().parent_path() / "bin/libwrap.so";
        expect(fatal(fs::exists(wrapso))) << "FAIL: no libwrap.so";
        auto loaded = s::run(*python, {"-I", "-c", "import ctypes, os, sys; assert ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW).wrap_call() == 8", wrapso.string()});
        expect(fatal(e2e::succeeded(loaded))) << "FAIL: a host that did not link core cannot load libwrap.so: " << s::text(loaded);
        expect(fatal(!s::contains_nocase(s::text(r), "also provided by"))) << "FAIL: a correct build printed a conflict warning";
        // half 2
        s::project alt(tmp / "alt", {{"mcpp.toml", "[package]\nname    = \"alt\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\nsources    = [\"src/*.c\"]\n[targets.alt]\nkind = \"shared\"\n"},
                                     {"src/alt.c", "int shared_answer(void) { return 999; }\nint alt_ping(void) { return shared_answer(); }\n"}});
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[build]\nc_standard = \"c11\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n[dependencies]\ncore = { path = \"../core\" }\nalt  = { path = \"../alt\" }\n");
        app.write("src/main.c", "#include <stdio.h>\nint shared_answer(void);\nint alt_ping(void);\nint main(void) { printf(\"%d %d\\n\", shared_answer(), alt_ping()); return 0; }\n");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        bool found = false;
        fs::path conflict_dir;
        for (const auto& rf : resolutions(app.at("target"))) {
            if (verdict(rf, "status") != "conflict") continue;
            conflict_dir = rf.parent_path();
            const auto doc = s::json_file(rf);
            nlohmann::json entry;
            for (const auto& en : doc["runtime"]["symbol_provision"])
                if (en.value("path", std::string()).ends_with("app")) { entry = en; break; }
            std::set<std::string> names, providers;
            bool all_func = true;
            for (const auto& c : entry["conflicts"]) {
                names.insert(c["symbol"].get<std::string>());
                for (const auto& p : c["also_provided_by"]) providers.insert(p.get<std::string>());
                if (c["kind"] != "func") all_func = false;
            }
            expect(fatal(names.contains("shared_answer"))) << "expected shared_answer, got " << entry["conflicts"].dump();
            expect(fatal(std::ranges::any_of(providers, [](const auto& p) { return p.contains("libalt.so"); }))) << "expected libalt.so among providers, got " << entry["conflicts"].dump();
            expect(fatal(all_func)) << entry["conflicts"].dump();
            found = true;
        }
        expect(fatal(found)) << "FAIL: two providers of shared_answer were not reported: " << s::text(r);
        const auto cbin = conflict_dir / "bin/app";
        expect(fatal(fs::is_regular_file(cbin))) << "FAIL: no executable in " << conflict_dir.string();
        const auto out = s::run(cbin);
        expect(fatal(s::has_line(out.out, "^7 7$"))) << "FAIL: expected '7 7' (the executable's copy winning), got: " << out.out;
        // --strict turns the finding into a failure.
        fs::last_write_time(app.at("src/main.c"), fs::file_time_type::clock::now());
        r = app.mcpp({"build", "--strict"});
        expect(fatal(!r)) << "FAIL: --strict did not fail on a reported conflict: " << s::text(r);
    };

    "323 post link record durability"_test[s::elf] = [] {
        // The post-link verdicts survive an invocation and the other command: `test` adds to
        // the record and `build` keeps the tests'; resolution.json publishes both on a warm
        // build; an artifact gone from the disk leaves the record.
        s::project p({{"mcpp.toml", "[package]\nname = \"dur\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                      {"src/main.cpp", "int main(){ return 0; }\n"},
                      {"tests/t_one.cpp", "int main(){ return 0; }\n"},
                      {"tests/t_two.cpp", "int main(){ return 0; }\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        std::optional<fs::path> ninja;
        for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { ninja = f; break; }
        expect(fatal(ninja.has_value())) << "FAIL: no build.ninja";
        const auto out = ninja->parent_path();
        const auto rec = out / ".mcpp-runtime-verdicts.json";
        const auto res = out / "resolution.json";
        expect(fatal(fs::is_regular_file(rec))) << "FAIL: no durable record was written";
        expect(fatal(e2e::contains(s::read_file(rec), "post_link_key"))) << "FAIL: the record carries no invalidation key";
        expect(fatal(e2e::contains(paths_in(rec, "loader_tags"), "bin/dur"))) << "FAIL: the package binary is not in the record after a build. got: " << paths_in(rec, "loader_tags");
        r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        for (std::string_view want : {"bin/dur", "bin/t_one", "bin/t_two"}) {
            expect(fatal(e2e::contains(paths_in(rec, "loader_tags"), want))) << "FAIL: after 'mcpp test' the record lost or never gained " << want << ". got: " << paths_in(rec, "loader_tags");
            expect(fatal(e2e::contains(paths_in(rec, "symbol_provision"), want))) << "FAIL: symbol_provision record is missing " << want << ". got: " << paths_in(rec, "symbol_provision");
        }
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        for (std::string_view want : {"bin/dur", "bin/t_one", "bin/t_two"})
            expect(fatal(e2e::contains(paths_in(rec, "loader_tags"), want))) << "FAIL: 'mcpp build' pruned the test binaries out of the record. got: " << paths_in(rec, "loader_tags");
        for (std::string_view name : {"loader_tags", "symbol_provision"}) {
            expect(fatal(e2e::contains(s::read_file(res), std::format("\"{}\"", name)))) << "FAIL: resolution.json does not publish " << name << " after a warm build";
            expect(fatal(e2e::contains(paths_in(res, name), "bin/dur"))) << "FAIL: resolution.json's " << name << " is empty on a warm build. got: '" << paths_in(res, name) << "'";
        }
        fs::remove(out / "bin/t_two");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto final_paths = paths_in(rec, "loader_tags");
        expect(fatal(s::lacks(final_paths, "bin/t_two"))) << "FAIL: a deleted artifact stayed in the record. got: " << final_paths;
        expect(fatal(e2e::contains(final_paths, "bin/dur"))) << "FAIL: pruning removed more than the deleted artifact. got: " << final_paths;
    };
}};

}  // namespace
