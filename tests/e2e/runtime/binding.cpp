// The runtime binding and what a link is checked against: a selected SubOS's environment reaching
// the program through the cached snapshot; the PT_INTERP/libc pair, a host library's verdict and
// a proven mismatch; the private libc kept out of the environment; a stored mismatch that a re-run
// does not turn green; the loader tags of an executable and a shared library; and the SubOS farm
// at load time (formerly tests/e2e/200, 206, 208, 209, 214, 220).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> first_named(const fs::path& root, std::string_view pattern) {
    for (const auto& f : s::find_files(root, pattern))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::optional<fs::path> first(const fs::path& root, std::string_view pattern) {
    const auto all = s::glob(root, pattern);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

// `stat -c '%y:%s'`: the write time and the size.
std::pair<fs::file_time_type, std::uintmax_t> stamp(const fs::path& f) {
    return {fs::last_write_time(f), fs::file_size(f)};
}

bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// The lines of `text` matching `pattern` and the `n` after each (`grep -A<n> -E`).
std::string with_following(std::string_view text, std::string_view pattern, std::size_t n) {
    std::vector<std::string> lines = split(text, '\n');
    const std::regex re{std::string(pattern)};
    std::string out;
    std::size_t until = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (std::regex_search(lines[i], re)) until = i + n + 1;
        if (i < until) out += lines[i] + "\n";
    }
    return out;
}

E2E binding{"binding", [] {
    "200 subos env reaches program"_test[s::elf] = [] {
        // One selected RuntimeBinding reaches the full-path and the cached `run`; the legacy
        // MCPP_SUBOS_DIR is ignored; a changed contract, an aged cache and no selection are
        // each taken as they are; a SubOS that cannot answer the contract is refused.
        const auto default_manifest = s::shared_home() / "registry/subos/default/.xlings.json";
        expect(fatal(fs::is_regular_file(default_manifest))) << "FAIL: mcpp default SubOS is missing";
        const auto default_subos = default_manifest.parent_path();
        std::string runtime;
        static const std::regex runtime_re{R"re(.*"runtime"[[:space:]]*:[[:space:]]*"([^"]*)".*)re"};
        for (const auto& line : split(s::read_file(default_manifest), '\n')) {
            std::smatch m;
            if (std::regex_match(line, m, runtime_re)) { runtime = m[1]; break; }
        }
        expect(fatal(!runtime.empty())) << "FAIL: default SubOS has no runtime identity";
        auto p = s::project::scaffold("hello");
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[xlings]\nsubos = \"probe\"\n");
        p.write("src/main.cpp", "#include <cstdio>\n#include <cstdlib>\nint main() {\n    const char* v = std::getenv(\"MCPP_E2E_PROBE\");\n    std::printf(\"PROBE=%s\\n\", v ? v : \"(unset)\");\n    return 0;\n}\n");
        const auto subos = p.dir() / ".mcpp/.xlings/subos/probe";
        const auto subos_text = subos.generic_string();
        fs::create_directories(subos / "usr/lib/dri");
        fs::create_directory_symlink(default_subos / "lib", subos / "lib");
        auto write_contract = [&](std::string_view suffix) {
            s::project(subos, {{".xlings.json", std::format(R"({{ "workspace": {{}},
  "subos_info": {{ "schema_version": 1, "runtime": "{}",
    "envs": {{ "probe@1": [
      {{ "var": "MCPP_E2E_PROBE", "op": "prepend",
        "value": "${{subosdir}}/usr/lib/{}" }} ] }} }} }}
)", runtime, suffix)}});
        };
        write_contract("dri");
        const auto probe = "PROBE=" + subos_text + "/usr/lib/dri";
        auto r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: first run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), probe))) << "FAIL: selected environment did not reach program: " << s::text(r);
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: cached run: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Resolving toolchain"))) << "FAIL: second run did not take fast path: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), probe))) << "FAIL: fast path changed the runtime environment: " << s::text(r);
        const auto evil = s::case_dir() / "evil";
        s::project(evil, {{".xlings.json", std::format(R"({{ "workspace": {{}}, "subos_info": {{ "schema_version": 1,
  "runtime": "{}", "envs": {{ "evil@1": [
    {{ "var": "MCPP_E2E_PROBE", "op": "set", "value": "EVIL" }} ] }} }} }}
)", runtime)}});
        r = p.mcpp({"run"}, {.env = {{"MCPP_SUBOS_DIR", evil.string()}}});
        expect(fatal(e2e::succeeded(r))) << "FAIL: run with ignored legacy variable: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), probe))) << "FAIL: MCPP_SUBOS_DIR overrode mcpp.toml: " << s::text(r);
        std::this_thread::sleep_for(1s);
        write_contract("dri2");
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: changed contract run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Resolving toolchain"))) << "FAIL: contract mutation did not invalidate fast path: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "PROBE=" + subos_text + "/usr/lib/dri2"))) << "FAIL: new snapshot was not applied: " << s::text(r);
        // `sed -i '/^\[xlings\]/,$d'`: the manifest up to its [xlings] line.
        auto manifest = p.read("mcpp.toml");
        const auto at = manifest.starts_with("[xlings]") ? 0 : manifest.find("\n[xlings]");
        if (at != std::string::npos) manifest.resize(at == 0 ? 0 : at + 1);
        p.write("mcpp.toml", manifest);
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: McppDefault run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "PROBE=(unset)"))) << "FAIL: named environment leaked into McppDefault: " << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[xlings]\nsubos = \"probe\"\n");
        write_contract("dri");
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run: " << s::text(r);
        const auto cache = p.at("target/.build_cache");
        expect(fatal(s::has_line(s::read_file(cache), "^runtimeBinding="))) << "FAIL: cache has no RuntimeBinding";
        std::string aged;
        for (const auto& line : split(s::read_file(cache), '\n'))
            if (!line.starts_with("runtimeBinding=")) aged += line + "\n";
        if (aged.ends_with("\n\n")) aged.pop_back();
        s::project(cache.parent_path(), {{".build_cache", aged}});
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: aged-cache run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Resolving toolchain"))) << "FAIL: cache without RuntimeBinding was replayed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), probe))) << "FAIL: aged cache rebuild lost selected environment: " << s::text(r);
        expect(fatal(s::has_line(s::read_file(cache), "^runtimeBinding="))) << "FAIL: rebuilt cache did not record RuntimeBinding";
        manifest = p.read("mcpp.toml");
        manifest.replace(manifest.find("subos = \"probe\""), 15, "subos = \"bare\"");
        p.write("mcpp.toml", manifest);
        s::project(p.dir() / ".mcpp/.xlings/subos/bare", {{".xlings.json", "{ \"workspace\": {} }\n"}});
        r = p.mcpp({"run"});
        expect(fatal(!r)) << "FAIL: named SubOS without RuntimeBinding was accepted: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "RuntimeBinding|no `subos_info`|does not describe itself"))) << "FAIL: missing-contract diagnostic is not actionable: " << s::text(r);
    };

    "206 runtime binding physics"_test[s::elf][s::gcc] = [] {
        // A host library linked under `allow_host_libs` builds and its verdict is what the
        // artifact's own search path decides; a hot no-op neither relinks nor re-probes; the
        // doctor reuses the verdict; a proven rule-B mismatch fails, also when cached.
        // `find /lib /usr/lib -name 'libtinfo.so' -print -quit`
        std::optional<fs::path> tinfo;
        for (const fs::path root : {"/lib", "/usr/lib"}) {
            std::error_code ec;
            if (tinfo || !fs::is_directory(root, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->path().filename() == "libtinfo.so") { tinfo = it->path(); break; }
        }
        expect(fatal(tinfo.has_value())) << "FAIL: host libtinfo development symlink is required";
        const auto tinfo_dir = tinfo->parent_path().generic_string();
        const auto tmp = s::case_dir();
        s::project safe(tmp / "safe", {{"mcpp.toml", std::format("[package]\nname = \"runtime-physics-safe\"\nversion = \"0.1.0\"\n\n[build]\nallow_host_libs = true\nldflags = [\"-L{}\", \"-ltinfo\"]\n", tinfo_dir)},
                                       {"src/main.cpp", "extern \"C\" int setupterm(const char*, int, int*);\nint main() {\n    int error = 0;\n    return setupterm(nullptr, 1, &error) == -99;\n}\n"}});
        auto r = safe.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: safe host-DSO build: " << s::text(r);
        const auto cache_lines = split(safe.read("target/.build_cache"), '\n');
        const fs::path output_dir = cache_lines.size() > 1 ? cache_lines[1] : std::string();
        expect(fatal(!output_dir.empty() && fs::is_directory(output_dir))) << "FAIL: build cache did not name output directory";
        const auto artifact = output_dir / "bin/runtime-physics-safe";
        const auto verdict = output_dir / ".mcpp-runtime-verdicts.json";
        expect(fatal(executable(artifact) && fs::is_regular_file(verdict))) << "FAIL: artifact/verdict missing";
        const auto h = s::read_header(artifact);
        expect(fatal(std::ranges::any_of(h.needed, [](const auto& n) { return n.contains("libtinfo.so"); }))) << "FAIL: safe control did not retain host libtinfo in DT_NEEDED";
        // Which verdict is owed is decided by the artifact's own runtime search path.
        const std::string soname = "libtinfo.so.6";
        std::string found;
        for (const auto& dir : split(h.runpath, ':'))
            if (!dir.empty() && fs::exists(fs::path(dir) / soname)) { found = dir + "/" + soname; break; }
        const auto doc = s::json_file(verdict);
        nlohmann::json entry = nlohmann::json::object();
        if (doc.contains("artifacts") && doc["artifacts"].contains("bin/runtime-physics-safe")) entry = doc["artifacts"]["bin/runtime-physics-safe"];
        const auto status = entry.contains("status") && entry["status"].is_string() ? entry["status"].get<std::string>() : std::string("null");
        if (!found.empty()) {
            expect(fatal(status == "pass")) << soname << " is at " << found << ", so the closure closes; the model says '" << status << "': " << doc.dump(2);
        } else {
            expect(fatal(status == "inconclusive")) << "nothing on the search path provides " << soname << "; the model says '" << status << "': " << doc.dump(2);
            std::string diagnostics;
            if (entry.contains("diagnostics"))
                for (const auto& d : entry["diagnostics"]) diagnostics += (d.is_string() ? d.get<std::string>() : d.dump()) + "\n";
            expect(fatal(e2e::contains(diagnostics, "libtinfo.so.6"))) << "the verdict does not name the library it could not resolve: " << doc.dump(2);
            expect(fatal(e2e::contains(diagnostics, "allow_host_libs"))) << "the verdict does not say why it stopped short of a proof: " << doc.dump(2);
        }
        const auto artifact_before = stamp(artifact), verdict_before = stamp(verdict);
        r = safe.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: hot no-op build: " << s::text(r);
        expect(fatal(stamp(artifact) == artifact_before)) << "FAIL: hot no-op relinked artifact";
        expect(fatal(stamp(verdict) == verdict_before)) << "FAIL: hot no-op rewrote/re-probed verdict";
        const auto doctor = s::text(safe.mcpp({"self", "doctor"}));
        expect(fatal(e2e::contains(doctor, "last runtime closure verdict"))) << "FAIL: doctor omitted stored runtime verdict: " << doctor;
        expect(fatal(s::has_line(doctor, "runtime-physics-safe.*" + status))) << "FAIL: doctor did not reuse the stored verdict ('" << status << "'): " << doctor;
        // A proven rule-B mismatch: the private libc RUNPATH with the host loader as PT_INTERP.
        s::project mismatch(tmp / "mismatch", {{"mcpp.toml", "[package]\nname = \"runtime-physics-mismatch\"\nversion = \"0.1.0\"\n\n[build]\nallow_host_libs = true\nldflags = [\"-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2\"]\n"},
                                               {"src/main.cpp", "int main() { return 0; }\n"}});
        r = mismatch.mcpp({"build"});
        auto out = s::text(r);
        expect(fatal(!r)) << "FAIL: Rule-B mismatch passed: " << out;
        expect(fatal(e2e::contains(out, "runtime closure validation failed"))) << "FAIL: missing runtime validation headline: " << out;
        expect(fatal(e2e::contains(out, "rule B"))) << "FAIL: missing Rule-B explanation: " << out;
        expect(fatal(e2e::contains(out, "ld-linux-x86-64.so.2"))) << "FAIL: diagnostic omitted actual interpreter: " << out;
        expect(fatal(e2e::contains(out, "mcpp.toml"))) << "FAIL: diagnostic omitted copyable remediation: " << out;
        const auto mismatch_verdict = s::find_file(mismatch.at("target"), ".mcpp-runtime-verdicts.json");
        expect(fatal(mismatch_verdict.has_value())) << "FAIL: mismatch verdict was not persisted";
        const auto mismatch_before = stamp(*mismatch_verdict);
        r = mismatch.mcpp({"build"});
        out = s::text(r);
        expect(fatal(!r)) << "FAIL: cached Rule-B mismatch became green: " << out;
        expect(fatal(e2e::contains(out, "rule B"))) << "FAIL: cached mismatch lost its explanation: " << out;
        expect(fatal(stamp(*mismatch_verdict) == mismatch_before)) << "FAIL: cached mismatch was reparsed/rewritten";
    };

    "208 private libc stays in the binary"_test[s::gcc][s::elf][s::unix_shell] = [] {
        // The private libc directory reaches the artifact's RUNPATH and not the program's
        // LD_LIBRARY_PATH: a /bin/sh child survives and the dlopen it serves still resolves.
        s::project app(s::case_dir() / "app", {{"runtime/plugin.c", "int runtime_plugin_answer(void) { return 42; }\n"},
                                               {"src/main.cpp", R"(#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

using answer_fn = int (*)();

int main() {
    const char* ldlp = std::getenv("LD_LIBRARY_PATH");
    std::printf("LDLP=[%s]\n", ldlp ? ldlp : "");

    std::FILE* pipe = ::popen("/bin/sh -c 'echo mcpp-child-ok'", "r");
    if (!pipe) { std::puts("CHILD=popen-failed"); return 10; }
    char buf[64] = {0};
    const bool got = std::fgets(buf, sizeof buf, pipe) != nullptr;
    const int status = ::pclose(pipe);
    buf[strcspn(buf, "\n")] = '\0';
    std::printf("CHILD=[%s] status=%d\n", got ? buf : "", status);
    if (!got || std::strcmp(buf, "mcpp-child-ok") != 0) return 11;

    void* handle = ::dlopen("libruntime_plugin.so", RTLD_NOW);
    if (!handle) { std::printf("DLOPEN=[%s]\n", ::dlerror()); return 12; }
    auto answer = reinterpret_cast<answer_fn>(
        ::dlsym(handle, "runtime_plugin_answer"));
    const int value = answer ? answer() : -1;
    ::dlclose(handle);
    std::printf("DLOPEN=ok answer=%d\n", value);
    return value == 42 ? 0 : 13;
}
)"},
                                               {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\nldflags = [\"-ldl\"]\n\n[runtime]\nlibrary_dirs = [\"runtime\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        // The host's C compiler makes the plugin, as the script's gcc did.
        const auto host_cc = s::host_cc();
        expect(fatal(host_cc.has_value())) << "no host C compiler links a shared object";
        auto cc = s::run(*host_cc, {"-shared", "-fPIC", "runtime/plugin.c", "-o", "runtime/libruntime_plugin.so"}, app.dir());
        expect(fatal(e2e::succeeded(cc))) << "the host gcc did not make the plugin: " << s::text(cc);
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto ninja = first_named(app.at("target"), R"(build\.ninja)");
        expect(fatal(ninja.has_value())) << "no build.ninja";
        const auto graph = s::read_file(*ninja);
        std::smatch m;
        static const std::regex glibc{R"(/[^ ",]*/xim-x-glibc/[0-9.]+/lib(64)?)"};
        if (!std::regex_search(graph, m, glibc)) skip("this toolchain has no private glibc payload");
        const auto glibc_lib = m[0].str();
        expect(fatal(e2e::contains(graph, "-Wl,-rpath," + glibc_lib))) << "FAIL: private libc directory is not in the artifact RUNPATH";
        r = app.mcpp({"run"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "run failed: " << out;
        std::string ldlp;
        for (const auto& l : split(out, '\n'))
            if (l.starts_with("LDLP=")) ldlp += l + "\n";
        expect(fatal(!ldlp.empty())) << "FAIL: program printed no LD_LIBRARY_PATH: " << out;
        expect(fatal(s::lacks(ldlp, glibc_lib))) << "FAIL: private libc directory reached LD_LIBRARY_PATH: " << ldlp;
        expect(fatal(e2e::contains(out, "CHILD=[mcpp-child-ok] status=0"))) << "FAIL: a /bin/sh child did not run cleanly under mcpp run (mcpp#401): " << out;
        expect(fatal(e2e::contains(out, "DLOPEN=ok answer=42"))) << "FAIL: dlopen through the runtime library dir regressed: " << out;
    };

    "209 runtime verdict never reruns green"_test[s::elf][s::gcc][s::python3] = [] {
        // A stored proven mismatch fails the next plain build and the doctor names it; a changed
        // artifact is validated again.
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "initial build failed: " << s::text(r);
        const auto cache = s::find_file(app.at("target"), ".mcpp-runtime-verdicts.json");
        if (!cache) skip("no glibc runtime binding on this host (no stored verdicts)");
        auto doc = s::json_file(*cache);
        expect(fatal(doc.contains("artifacts") && doc["artifacts"].is_object() && !doc["artifacts"].empty())) << "no artifacts recorded in the verdict cache";
        for (auto it = doc["artifacts"].begin(); it != doc["artifacts"].end(); ++it)
            expect(fatal(it.value().value("status", std::string()) == "pass")) << "expected every artifact to pass on a clean build, got " << it.key() << ": " << it.value().dump();
        // 1
        for (auto& v : doc["artifacts"]) {
            v["status"] = "proven_mismatch";
            v["diagnostics"] = nlohmann::json::array({"synthetic mismatch recorded by 209"});
        }
        s::project(cache->parent_path(), {{cache->filename().string(), doc.dump(2)}});
        r = app.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: a stored proven mismatch went green on a plain re-run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "runtime closure validation failed"))) << "FAIL: the re-run failed, but not for the recorded runtime mismatch: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "synthetic mismatch recorded by 209"))) << "FAIL: the stored diagnostics were not reported to the user: " << s::text(r);
        // 2
        const auto doctor = s::text(app.mcpp({"self", "doctor"}));
        expect(fatal(s::contains_nocase(doctor, "mismatch"))) << "FAIL: doctor did not surface the stored runtime mismatch: " << doctor;
        // 3
        app.write("src/main.cpp", "int main() { return 0; }\n// touched so the artifact is relinked and the verdict is re-derived\n");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: a rebuilt artifact must be re-validated, not stay condemned: " << s::text(r);
        doc = s::json_file(*cache);
        expect(fatal(doc.contains("artifacts") && !doc["artifacts"].empty())) << "verdict cache lost its artifacts after the rebuild";
        for (auto it = doc["artifacts"].begin(); it != doc["artifacts"].end(); ++it)
            expect(fatal(it.value().value("status", std::string()) == "pass")) << it.key() << ": " << it.value().dump();
    };

    "214 executable carries dt rpath"_test[s::gcc][s::elf][s::python3] = [] {
        // An executable carries DT_RPATH and a shared library keeps the linker's DT_RUNPATH; the
        // flag is on the executable's link edge only; rule E records both and the record holds
        // across a no-op build and a build that relinks one of the two.
        s::project p({{"mcpp.toml", "[package]\nname = \"tags\"\nversion = \"0.1.0\"\n\n[targets.taglib]\nkind = \"shared\"\n\n[targets.tagbin]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/lib.cppm", "export module tags.lib;\nexport int tag_value() { return 7; }\n"},
                      {"src/main.cpp", "import tags.lib;\nint main() { return tag_value() == 7 ? 0 : 1; }\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto ninja = first(p.at("target"), "*/*/build.ninja");
        expect(fatal(ninja.has_value())) << "FAIL: no build.ninja";
        const auto graph = s::read_file(*ninja);
        const auto bin_edge = s::lines_with(with_following(graph, "^build bin/tagbin *: *cxx_link", 4), "unit_ldflags");
        const auto lib_edge = s::lines_with(with_following(graph, R"(^build bin/libtaglib\.so.* : *cxx_shared)", 4), "unit_ldflags");
        auto joined = [](const std::vector<std::string>& v) { std::string o; for (const auto& l : v) o += l + "\n"; return o; };
        expect(fatal(e2e::contains(joined(bin_edge), "--disable-new-dtags"))) << "FAIL: the executable's link edge carries no loader-tag flag: " << joined(bin_edge);
        expect(fatal(s::lacks(joined(lib_edge), "--disable-new-dtags"))) << "FAIL: the shared library's link edge carries the executable's flag: " << joined(lib_edge);
        const auto bin = first(p.at("target"), "*/*/bin/tagbin");
        const auto lib = first(p.at("target"), "*/*/bin/libtaglib.so");
        expect(fatal(bin.has_value())) << "FAIL: no executable produced";
        expect(fatal(lib.has_value())) << "FAIL: no shared library produced";
        // 214's tag reader: the form (PT_INTERP) and the tags of the whole dynamic section.
        auto read_tag = [](const fs::path& f) {
            const auto h = s::read_header(f);
            if (h.kind != s::format::elf || !h.wide) return std::string("NOT-ELF64");
            const std::string form = h.elf_interpreter ? "executable" : "shared_library";
            const std::string tag = h.elf_rpath && h.elf_runpath ? "BOTH" : h.elf_rpath ? "RPATH" : h.elf_runpath ? "RUNPATH" : "NONE";
            return form + " " + tag;
        };
        const auto bin_tag = read_tag(*bin), lib_tag = read_tag(*lib);
        expect(fatal(bin_tag == "executable RPATH")) << "FAIL: executable must carry DT_RPATH, got: " << bin_tag;
        expect(fatal(lib_tag == "shared_library RUNPATH" || lib_tag == "shared_library NONE")) << "FAIL: shared library came out as " << lib_tag;
        const auto res = first(p.at("target"), "*/*/resolution.json");
        expect(fatal(res.has_value())) << "FAIL: no resolution.json";
        const auto record = s::read_file(*res);
        expect(fatal(e2e::contains(record, "\"loader_tags\""))) << "FAIL: resolution.json records no loader_tags — rule E did not run";
        expect(fatal(s::no_line(record, R"("status": *"violation")"))) << "FAIL: rule E reported a violation on mcpp's own output";
        const auto doc = s::json(record);
        std::map<std::string, nlohmann::json> by_form;
        for (const auto& t : doc["runtime"]["loader_tags"]) by_form[t.value("form", std::string())] = t;
        for (const auto& [form, want] : {std::pair{"executable", "DT_RPATH"}, std::pair{"shared_library", "DT_RUNPATH"}}) {
            expect(fatal(by_form.contains(form))) << "FAIL: rule E recorded no " << form;
            const auto& t = by_form[form];
            expect(fatal(t.value("status", std::string()) == "ok" && t.value("required", std::string()) == want)) << "FAIL: " << form << ": " << t.dump();
        }
        auto count_tags = [&] { return s::json_file(*res)["runtime"]["loader_tags"].size(); };
        const auto before = count_tags();
        expect(fatal(before >= 2u)) << "FAIL: rule E recorded only " << before << " artifact(s)";
        // (a) a build that relinks nothing
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(count_tags() == before)) << "FAIL: a no-op build changed the loader-tag record: " << before << " -> " << count_tags();
        // (b) a build that relinks one of the two
        std::this_thread::sleep_for(1s);
        p.write("src/main.cpp", p.read("src/main.cpp") + "\n// touch\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(count_tags() == before)) << "FAIL: relinking one artifact shrank the loader-tag record: " << before << " -> " << count_tags();
        r = p.mcpp({"run", "tagbin"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
    };

    "220 farm only library runs"_test[s::gcc][s::elf][s::python3] = [] {
        // A library only the SubOS provides loads (a farm the case owns, then this machine's
        // farm), and a DT_NEEDED nothing provides fails the build naming itself.
        const auto tmp = s::case_dir();
        // half 0: a farm this case owns
        const auto farm_own = tmp / "ownfarm/.mcpp/.xlings/subos/probe/lib";
        fs::create_directories(farm_own);
        s::project probelib(tmp / "probelib", {{"mcpp.toml", "[package]\nname = \"probelib\"\nversion = \"0.1.0\"\n\n[targets.mcppfarmprobe]\nkind = \"shared\"\n"},
                                               {"src/probe.cppm", "export module probelib.probe;\nexport int mcpp_farm_probe() { return 42; }\n"}});
        auto r = probelib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: probe library build: " << s::text(r);
        const auto probe_so = first(probelib.at("target"), "*/*/bin/libmcppfarmprobe.so");
        expect(fatal(probe_so.has_value())) << "FAIL: no probe .so";
        fs::copy_file(*probe_so, farm_own / "libmcppfarmprobe.so");
        const auto farm = farm_own.generic_string();
        s::project own(tmp / "ownfarm", {{"mcpp.toml", std::format("[package]\nname = \"ownfarm\"\nversion = \"0.1.0\"\n\n[xlings]\nsubos = \"probe\"\n\n[build]\nallow_host_libs = true\n"
                                                                   "ldflags = [\"-Wl,--no-as-needed\", \"-L{}\", \"-lmcppfarmprobe\", \"-Wl,--as-needed\"]\n", farm)},
                                         {"src/main.cpp", "int main() { return 0; }\n"}});
        r = own.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: build against a SubOS-provided library: " << s::text(r);
        const auto own_bin = first(own.at("target"), "*/*/bin/ownfarm");
        expect(fatal(own_bin.has_value())) << "FAIL: no artifact";
        auto ran = s::run(*own_bin);
        expect(fatal(e2e::succeeded(ran))) << "FAIL: an artifact could not load a library its own SubOS provides (exit " << ran.status << "): " << s::text(ran);
        // A library only this machine's farm provides, derived from the recorded closure.
        s::project probe(tmp / "probe", {{"mcpp.toml", "[package]\nname = \"probe\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        r = probe.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto res = first(probe.at("target"), "*/*/resolution.json");
        expect(fatal(res.has_value())) << "no resolution.json";
        const auto doc = s::json_file(*res);
        std::vector<std::string> farms, payloads;
        if (doc["runtime"].contains("search") && doc["runtime"]["search"].contains("closure"))
            for (const auto& d : doc["runtime"]["search"]["closure"])
                (d.value("origin", std::string()) == "subos_farm" ? farms : payloads).push_back(d.value("path", std::string()));
        std::set<std::string> provided;
        for (const auto& p : payloads) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(p, ec)) provided.insert(e.path().filename().string());
        }
        static const std::regex runtime_lib{R"(^lib(c|m|dl|rt|pthread|gcc_s|stdc\+\+|atomic|c\+\+.*)\.so)"};
        std::string farm_lib, farm_dir;
        for (const auto& d : farms) {
            std::error_code ec;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(d, ec)) names.push_back(e.path().filename().string());
            if (ec) continue;
            std::ranges::sort(names);
            for (const auto& n : names) {
                if (!n.ends_with(".so") || !n.starts_with("lib")) continue;
                if (std::regex_search(n, runtime_lib)) continue;
                if (provided.contains(n)) continue;
                const auto base = n.substr(0, n.size() - 3);
                if (!fs::exists(fs::path(d) / (base + ".so.1")) && !fs::is_symlink(fs::path(d) / n)) continue;
                farm_lib = base.substr(3);
                farm_dir = d;
                break;
            }
            if (!farm_lib.empty()) break;
        }
        if (farm_lib.empty())
            skip("this SubOS farm provides no library the payloads do not already provide, so there is nothing whose loading proves the farm is reachable");
        // half 1: it links and runs
        s::project uses(tmp / "uses", {{"mcpp.toml", std::format("[package]\nname = \"uses\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,--no-as-needed\", \"-l{}\", \"-Wl,--as-needed\"]\n", farm_lib)},
                                       {"src/main.cpp", "int main() { return 0; }\n"}});
        r = uses.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: linking against a farm-provided library failed: " << s::text(r);
        const auto bin = first(uses.at("target"), "*/*/bin/uses");
        expect(fatal(bin.has_value())) << "no bin/uses";
        const auto want = "lib" + farm_lib + ".so";
        const auto needed = s::read_header(*bin).needed;
        expect(fatal(std::ranges::any_of(needed, [&](const auto& n) { return n.starts_with(want); }))) << "FAIL: " << want << " is not DT_NEEDED — --no-as-needed did not hold";
        ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "FAIL: the artifact could not start (exit " << ran.status << ") with -l" << farm_lib << " from " << farm_dir;
        // half 2: an unsatisfiable DT_NEEDED turns the build red
        s::project ghost(tmp / "ghostlib", {{"mcpp.toml", "[package]\nname = \"ghostlib\"\nversion = \"0.1.0\"\n\n[targets.ghost]\nkind = \"shared\"\n\n[build]\nldflags = [\"-Wl,-soname,libmcpp_ghost_probe.so.1\"]\n"},
                                            {"src/ghost.cppm", "export module ghostlib.ghost;\nexport int ghost_value() { return 1; }\n"}});
        r = ghost.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: ghost library build: " << s::text(r);
        const auto ghost_so = first(ghost.at("target"), "*/*/bin/libghost.so");
        expect(fatal(ghost_so.has_value())) << "FAIL: no libghost.so produced";
        const auto ghost_dir = fs::absolute(ghost_so->parent_path()).generic_string();
        s::project needs(tmp / "needsghost", {{"mcpp.toml", std::format("[package]\nname = \"needsghost\"\nversion = \"0.1.0\"\n\n[build]\nldflags = [\"-Wl,--no-as-needed\", \"-L{}\", \"-lghost\", \"-Wl,--as-needed\"]\n", ghost_dir)},
                                              {"src/main.cpp", "int main() { return 0; }\n"}});
        r = needs.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: a DT_NEEDED that nothing can provide built successfully: " << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "libmcpp_ghost_probe.so.1"))) << "FAIL: the build failed, but not for the unresolvable DT_NEEDED: " << s::text(r);
    };
}};

}  // namespace
