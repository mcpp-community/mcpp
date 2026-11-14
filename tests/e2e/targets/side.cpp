// The target side: which layer comes from where and how the report shows it, the rules a layer's
// supplier is held to, the environment a declaration puts in front, a package that names a layer
// keeps the target's compiler, a graph-supplied layer reaches every unit, the C ABI environment a C
// library declares, a declared absence explains a link, host-only layers do not select the target
// ABI, and suppliers that require one another are refused (formerly tests/e2e/268, 280, 281, 290,
// 292, 304, 741, 744, 1014, 1055).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The first match of `pattern`'s first group in `text`, or "".
std::string first_group(const std::string& text, const std::regex& pattern) {
    std::smatch m;
    return std::regex_search(text, m, pattern) ? m[1].str() : std::string();
}

// A compile database's entry's words: its `arguments`, else its `command` split at spaces.
std::vector<std::string> words_of(const nlohmann::json& entry) {
    std::vector<std::string> out;
    if (entry.contains("arguments") && entry["arguments"].is_array() && !entry["arguments"].empty()) {
        for (const auto& a : entry["arguments"]) out.push_back(a.get<std::string>());
        return out;
    }
    const auto command = entry.value("command", std::string());
    for (auto part : std::views::split(std::string_view(command), ' ')) out.emplace_back(part.begin(), part.end());
    return out;
}

bool any_word_contains(const nlohmann::json& entry, std::string_view needle) {
    return std::ranges::any_of(words_of(entry), [&](const std::string& w) { return w.contains(needle); });
}

E2E side{"side", [] {
    "268 target side resolution"_test[s::unix_shell] = [] {
        // A layer a dependency declares is reported with its interface, its supplier and its
        // origin; a package that declares nothing fills nothing; a misspelt name in the mcpp:
        // namespace is reported and fills nothing; a name outside it passes untouched.
        s::project p({{"mcpp.toml", "[package]\nname    = \"target-side-probe\"\nversion = \"0.1.0\"\n\n[dependencies]\nfake-system = { path = \"sys\" }\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"},
                      {"sys/src/sys.c", "int fake_system_marker(void) { return 0; }\n"}});
        auto provider_declares = [&](std::vector<std::string> caps) {
            std::string m = "[package]\nname     = \"fake-system\"\nversion  = \"2.0.0\"\n";
            if (!caps.empty()) {
                m += "provides = [";
                for (std::size_t i = 0; i < caps.size(); ++i) m += std::format("{}\"{}\"", i ? ", " : "", caps[i]);
                m += "]\n";
            }
            m += "\n[targets.fake-system]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n";
            p.write("sys/mcpp.toml", m);
            p.remove("target");
        };
        provider_declares({"mcpp:kernel-abi=fakeos", "mcpp:c-abi=fakelibc"});
        const auto out = s::text(p.mcpp({"build"}));
        expect(fatal(s::has_line(out, "kernel-abi +fakeos"))) << "the kernel-abi layer must report the interface the package declared";
        expect(fatal(s::has_line(out, "c-abi +fakelibc"))) << "the c-abi layer must report the interface the package declared";
        expect(fatal(s::has_line(out, R"(fake-system@2\.0\.0)"))) << "the report must name the providing package and its version";
        expect(fatal(e2e::contains(out, "graph"))) << "a layer supplied by a dependency must be reported as coming from the graph: " << out;
        expect(fatal(s::has_line(out, R"(c\+\+-abi *—)"))) << "a project with no C++ runtime package must report that layer absent";
        provider_declares({});
        const auto plain = s::text(p.mcpp({"build"}, {.env = {{"MCPP_VERBOSE", "1"}}}));
        expect(fatal(s::lacks(plain, "fakeos"))) << "a package that declares no capability must not fill a layer";
        expect(fatal(s::has_line(plain, "kernel-abi .*payload"))) << "with nothing in the graph, the layers come from the payload";
        provider_declares({"mcpp:kernel_abi=fakeos", "mcpp:c-abi=fakelibc"});
        const auto bad = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(bad, "kernel_abi"))) << "a misspelled capability in the mcpp: namespace must be reported: " << bad;
        expect(fatal(e2e::contains(bad, "mcpp:kernel-abi"))) << "the report must list the layer names that do exist: " << bad;
        expect(fatal(s::no_line(bad, "kernel-abi +fakeos"))) << "a name this engine does not know must not fill a layer";
        expect(fatal(s::has_line(bad, "c-abi +fakelibc"))) << "a correctly spelled capability beside a misspelled one must still resolve";
        provider_declares({"a-capability-mcpp-never-heard-of", "mcpp:c-abi=fakelibc"});
        const auto free = s::text(p.mcpp({"build"}));
        expect(fatal(s::lacks(free, "names no capability mcpp knows"))) << "a capability outside the mcpp: namespace must pass through untouched: " << p.read("sys/mcpp.toml");
        expect(fatal(s::has_line(free, "c-abi +fakelibc"))) << "the layer beside the unknown name must still resolve";
    };

    "280 target side layers"_test = [] {
        // The target side is five layers; a zero-configuration build prints none of them (they
        // came from the compiler payload), MCPP_VERBOSE prints all five, and the compiler layer
        // names a family.
        auto p = s::project::scaffold("plain");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto out = s::text(r);
        expect(fatal(s::has_line(out, R"(^\s+Target )"))) << "no Target line at all";
        static constexpr std::string_view labels[] = {"compiler", "compiler-runtime", "kernel-abi", "c-abi", "c++-abi"};
        for (auto label : labels)
            expect(fatal(s::lacks(out, std::format("             {} ", label)))) << "a zero-configuration build printed the '" << label << "' layer, which came from the compiler payload and is therefore not news";
        (void)p.mcpp({"clean"});
        r = p.mcpp({"build"}, {.env = {{"MCPP_VERBOSE", "1"}}});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto verbose = s::text(r);
        for (auto label : labels)
            expect(fatal(e2e::contains(verbose, std::format("             {} ", label)))) << "MCPP_VERBOSE did not print the '" << label << "' layer: " << verbose;
        expect(fatal(s::has_line(verbose, "compiler +(llvm|gcc|msvc) "))) << "the compiler layer must report a family, not a driver name";
    };

    "281 target side rules"_test[s::gcc] = [] {
        // Each layer has one supplier, the compiler is no package's to supply, a requirement the
        // compiler does not meet is refused with its remedy, and a layer name this engine does not
        // know is ignored in a dependency and refused in the root.
        const auto tmp = s::case_dir();
        s::project(tmp / "runtime", {{"mcpp.toml", "[package]\nnamespace = \"probe\"\nname      = \"needs-llvm\"\nversion   = \"0.1.0\"\nprovides  = [\"mcpp:c++-abi=libc++\"]\nrequires  = [\"mcpp:compiler=llvm\"]\n\n[build]\nsources = []\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nneeds-llvm = { path = \"../runtime\" }\n"},
                                     {"src/main.cpp", "int main(){ return 0; }\n"}});
        auto r = app.mcpp({"build"}, {.env = {{"MCPP_TOOLCHAIN", "gcc@16.1.0"}}});
        auto out = s::text(r);
        expect(fatal(!r)) << "a requirement gcc does not meet was accepted: " << out;
        expect(fatal(e2e::contains(out, "requires the compiler to be"))) << "the refusal does not name the requirement: " << out;
        expect(fatal(e2e::contains(out, "remove it"))) << "the refusal names no next step: " << out;
        expect(fatal(s::lacks(out, "mcpp toolchain default"))) << "the refusal offers a global change that would not fix this failure";
        expect(fatal(s::has_line(out, R"(^\s+compiler\s+gcc)"))) << "the refusal omits the layer it rests on";
        for (std::string n : {"a", "b"})
            s::project(tmp / n, {{"mcpp.toml", std::format("[package]\nnamespace = \"probe\"\nname      = \"libc-{0}\"\nversion   = \"0.1.0\"\nprovides  = [\"mcpp:c-abi=tiny{0}\"]\n\n[build]\nsources = []\n", n)}});
        s::project app2(tmp / "app2", {{"mcpp.toml", "[package]\nname    = \"app2\"\nversion = \"0.1.0\"\n\n[dependencies]\nlibc-a = { path = \"../a\" }\nlibc-b = { path = \"../b\" }\n"},
                                       {"src/main.cpp", "int main(){ return 0; }\n"}});
        r = app2.mcpp({"build"});
        out = s::text(r);
        expect(fatal(!r)) << "two suppliers of one layer were accepted: " << out;
        expect(fatal(e2e::contains(out, "two packages supply the c-abi"))) << "the refusal does not name the layer: " << out;
        for (std::string_view pkg : {"libc-a@0.1.0", "libc-b@0.1.0"})
            expect(fatal(e2e::contains(out, pkg))) << "the refusal does not name " << pkg << ": " << out;
        s::project(tmp / "c", {{"mcpp.toml", "[package]\nnamespace = \"probe\"\nname      = \"claims-compiler\"\nversion   = \"0.1.0\"\nprovides  = [\"mcpp:compiler=llvm\"]\n\n[build]\nsources = []\n"}});
        s::project app3(tmp / "app3", {{"mcpp.toml", "[package]\nname    = \"app3\"\nversion = \"0.1.0\"\n\n[dependencies]\nclaims-compiler = { path = \"../c\" }\n"},
                                       {"src/main.cpp", "int main(){ return 0; }\n"}});
        r = app3.mcpp({"build"});
        out = s::text(r);
        expect(fatal(!r)) << "a package was allowed to supply the compiler: " << out;
        expect(fatal(e2e::contains(out, "not a layer a package can supply"))) << "the refusal does not say why: " << out;
        s::project(tmp / "future", {{"mcpp.toml", "[package]\nnamespace = \"probe\"\nname      = \"from-the-future\"\nversion   = \"0.1.0\"\nprovides  = [\"mcpp:quantum-abi=spooky\"]\n\n[build]\nsources = []\n"}});
        s::project app4(tmp / "app4", {{"mcpp.toml", "[package]\nname    = \"app4\"\nversion = \"0.1.0\"\n\n[dependencies]\nfrom-the-future = { path = \"../future\" }\n"},
                                       {"src/main.cpp", "int main(){ return 0; }\n"}});
        r = app4.mcpp({"build"});
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "a dependency naming a layer this engine does not know must not fail the build: " << out;
        expect(fatal(e2e::contains(out, "quantum-abi"))) << "the ignored layer must be named: " << out;
        app4.write("mcpp.toml", "[package]\nname     = \"app4\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:quantum-abi=spooky\"]\n");
        r = app4.mcpp({"build"});
        out = s::text(r);
        expect(fatal(!r)) << "a misspelled layer name in the root manifest must be an error: " << out;
        expect(fatal(e2e::contains(out, "names no capability mcpp knows"))) << "the refusal does not say what is wrong: " << out;
    };

    "290 the declaration puts an environment in front"_test[s::gcc][s::unix_shell] = [] {
        // A build program finds the tool the project declared: a project that declares nothing
        // gets the PATH mcpp was started with, and `[xlings] subos` puts that SubOS's bin first,
        // the inherited PATH unchanged behind it.
        const auto tmp = s::case_dir();
        constexpr std::string_view probe = R"(import std;
int main() {
    const char* p = std::getenv("PATH");
    std::println("PROBE_PATH={}", p ? p : "");
    return 1;
}
)";
        auto make_project = [&](std::string_view dir, std::string_view xlings) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"pathprobe\"\nversion = \"0.1.0\"\n{}", xlings.empty() ? std::string() : std::format("\n{}\n", xlings))},
                                          {"src/main.cpp", "int main() { return 0; }\n"},
                                          {"build.mcpp", probe}});
        };
        static const std::regex path_line{"PROBE_PATH=(.*)"};
        auto run_probe = [&](const s::project& p) { return first_group(s::text(p.mcpp({"build"})), path_line); };
        const auto inherited = s::env("PATH").value_or("");
        const auto plain = run_probe(make_project("plain", ""));
        if (plain.empty()) skip("the build program did not report — it may not have run here");
        expect(fatal(plain == inherited)) << "FAIL: a project that declared no environment had its PATH changed\n  inherited: " << inherited << "\n  got:       " << plain;
        const auto declared = run_probe(make_project("declared", "[xlings]\nsubos = \"default\""));
        if (declared.empty()) skip("the declaring project's build program did not report");
        expect(fatal(declared != inherited)) << "FAIL: the declaration changed nothing — the environment is not in front";
        const auto colon = declared.find(':');
        const auto first = declared.substr(0, colon);
        expect(fatal(std::regex_match(first, std::regex{".*/subos/.*/bin"}))) << "FAIL: something other than a subos was prepended: " << first;
        const auto rest = colon == std::string::npos ? declared : declared.substr(colon + 1);
        expect(fatal(rest == inherited)) << "FAIL: the inherited PATH was not preserved behind the prefix\n  inherited: " << inherited << "\n  got:       " << rest;
    };

    "292 a package that names a layer does not lose the targets compiler"_test[s::llvm][s::unix_shell] = [] {
        // Naming a layer changes the system, not the compiler that emits the target: the
        // bare-metal row keeps llvm, a hosted row keeps the project's toolchain, and a kernel-abi
        // package over the system's C library leaves the row's convention standing.
        const auto tmp = s::case_dir();
        const auto llvm = s::llvm_version();
        static const std::regex resolved_re{R"(Resolved ([a-z]+@[0-9.]+))"};
        auto resolved = [&](const s::project& p, std::string_view target) {
            return first_group(s::text(p.mcpp({"build", "--target", std::string(target)})), resolved_re);
        };
        auto make_project = [&](std::string_view dir, std::string_view provides) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"layerprobe\"\nversion = \"0.1.0\"\n{}", provides.empty() ? std::string() : std::format("{}\n", provides))},
                                          {"src/main.cpp", "extern \"C\" void _start() { for (;;) {} }\n"}});
        };
        const auto base = resolved(make_project("plain", ""), "riscv64-none-elf");
        if (!base.starts_with("llvm@")) skip(std::format("the bare-metal row did not resolve to llvm here (got '{}')", base.empty() ? "nothing" : base));
        const auto after = resolved(make_project("declares", "provides = [\"mcpp:kernel-abi=openkal\"]"), "riscv64-none-elf");
        expect(fatal(after == base)) << "FAIL: naming a layer changed the target's compiler\n  without provides: " << base << "\n  with provides:    " << (after.empty() ? "nothing" : after);
        s::project hosted(tmp / "hosted", {{"mcpp.toml", std::format("[package]\nname     = \"hostedprobe\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:c-abi=musl\"]\n\n[toolchain]\ndefault = \"llvm@{}\"\n", llvm)},
                                           {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto h = resolved(hosted, "x86_64-linux-musl");
        if (h.empty())
            std::println("SKIP: the hosted project did not report a resolution here");
        else
            expect(fatal(h.starts_with("llvm@"))) << "FAIL: the target row replaced the toolchain this project chose: chose llvm@" << llvm << ", resolved " << h;
        s::project kabi(tmp / "kabi", {{"mcpp.toml", std::format("[package]\nname     = \"kabiprobe\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=openkal\"]\n\n[toolchain]\ndefault = \"llvm@{}\"\n", llvm)},
                                       {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto k = s::text(kabi.mcpp({"build", "--target", "x86_64-linux-musl"}));
        expect(fatal(e2e::contains(k, "takes its C library from"))) << "FAIL: a kernel-abi package over the system's C library cancelled the row's convention: " << k;
    };

    "304 the target side reaches every unit"_test[s::unix_shell][s::jq] = [] {
        // A layer the graph supplies is beneath every unit, a sibling package's included; its
        // private directory reaches only its own units; a non-layer package's directory does not
        // spread.
        s::project p({{"mcpp.toml", "[package]\nname    = \"reachprobe\"\nversion = \"0.1.0\"\n\n[dependencies]\nabiprov = { path = \"abi\" }\nsibling = { path = \"dep\" }\n"},
                      {"src/main.cpp", "#include <stdio.h>\nimport abiprov;\nimport sibling;\nint main() { printf(\"%d\\n\", abiprov_v() + sibling_v()); }\n"},
                      {"abi/mcpp.toml", "[package]\nname    = \"abiprov\"\nversion = \"0.1.0\"\nprovides = [\"mcpp:c++-abi=libc++\"]\n\n[build]\ninclude_dirs         = [\"abi-internal\", \"abi-include\"]\nprivate_include_dirs = [\"abi-internal\"]\n"},
                      {"abi/src/abiprov.cppm", "export module abiprov;\nexport int abiprov_v() { return 1; }\n"},
                      {"abi/abi-include/marker.h", "#pragma once\n"},
                      {"abi/abi-internal/private.h", "#pragma once\n"},
                      {"dep/mcpp.toml", "[package]\nname    = \"sibling\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"dep-include\"]\n"},
                      {"dep/src/sibling.cppm", "export module sibling;\nexport int sibling_v() { return 2; }\n"},
                      {"dep/dep-include/own.h", "#pragma once\n"}});
        const s::options o{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        auto r = p.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the probe project did not build: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json") && fs::file_size(p.at("compile_commands.json")) > 0)) << "FAIL: no compile_commands.json";
        const auto cdb = s::json_file(p.at("compile_commands.json"));
        auto file_of = [](const nlohmann::json& e) { return e.value("file", std::string()); };
        std::size_t siblings = 0, own_private = 0;
        std::vector<std::string> missing, leaked, private_leak;
        for (const auto& e : cdb) {
            const auto f = file_of(e);
            const bool in_dep = f.contains("/dep/src/"), in_abi = f.contains("/abi/src/");
            if (in_dep) {
                ++siblings;
                if (!any_word_contains(e, "abi-include")) missing.push_back(f);
            }
            if (in_abi && any_word_contains(e, "dep-include")) leaked.push_back(f);
            if (!in_abi && any_word_contains(e, "abi-internal")) private_leak.push_back(f);
            if (in_abi && any_word_contains(e, "abi-internal")) ++own_private;
        }
        expect(fatal(siblings >= 1u)) << "FAIL: the CDB has no sibling-package row — nothing was checked";
        expect(fatal(missing.empty())) << "FAIL: the target side did not reach " << siblings << " sibling unit(s): " << nlohmann::json(missing).dump();
        expect(fatal(leaked.empty())) << "FAIL: a non-layer package's directory reached a sibling — the fix is too wide: " << nlohmann::json(leaked).dump();
        expect(fatal(private_leak.empty())) << "FAIL: the layer's PRIVATE directory reached units outside the package: " << nlohmann::json(private_leak).dump();
        expect(fatal(own_private >= 1u)) << "FAIL: the provider lost its own private directory — the check above proves nothing";
    };

    "741 c abi environment declared by the c library"_test[s::llvm][s::unix_shell] = [] {
        // A C library's `[c-abi]` declaration is realised as the compiler's target and flags for
        // every unit of the graph (assembly included) but the kernel-abi provider's, and a request
        // the target cannot realise refuses the build, naming the target and the request.
        const auto llvm = s::llvm_version();
        s::project p({{"fakemusl/src/lib.c", "int fakemusl_marker(void) { return 0; }\n"},
                      {"openkalwin/src/shim.c", "int openkalwin_marker(void) { return 0; }\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"},
                      {"src/probe.S", ".text\n.globl cabi_probe_asm_marker\ncabi_probe_asm_marker:\n    ret\n"},
                      {"mcpp.toml", "[package]\nname    = \"cabi-probe\"\nversion = \"0.1.0\"\n\n[dependencies]\nfakemusl  = { path = \"fakemusl\" }\nopenkalwin = { path = \"openkalwin\" }\n\n[targets.cabi-probe]\nkind    = \"lib\"\nsources = [\"src/*.cpp\", \"src/*.S\"]\n\n[build]\nallow_host_libs = true\n"},
                      {"openkalwin/mcpp.toml", "[package]\nname          = \"openkalwin\"\nversion       = \"1.0.0\"\nprovides      = [\"mcpp:kernel-abi=openkal\"]\n\n[targets.openkalwin]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n"}});
        p.write("fakemusl/mcpp.toml", "[package]\nname     = \"fakemusl\"\nversion  = \"1.0.0\"\nprovides = [\"mcpp:c-abi=fakemusl\"]\n\n[targets.fakemusl]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n\n[c-abi]\npresents   = \"posix\"\ndata-model = \"arch-default\"\nwchar      = 32\nbuiltins   = \"iso\"\n");
        p.remove("target");
        const std::vector<std::string> target_args{"--target", "x86_64-windows-gnu", "--toolchain", "llvm@" + llvm};
        auto with = [&](std::vector<std::string> args) {
            args.insert(args.end(), target_args.begin(), target_args.end());
            return args;
        };
        auto r = p.mcpp(with({"build"}));
        expect(fatal(e2e::succeeded(r))) << "FAIL: a realisable [c-abi] request must not fail the build: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "c-abi +fakemusl"))) << "FAIL: the target-side report must name the c-abi provider";
        r = p.mcpp(with({"emit", "build-database", "--format", "json"}));
        expect(fatal(e2e::succeeded(r))) << "FAIL: emit build-database must succeed on the realised graph: " << r.err;
        const auto doc = s::json(r.out);
        // `" ".join(" ".join(args) for each unit whose source contains <s>)`.
        auto joined = [&](std::string_view source_contains) {
            std::string out;
            std::size_t units = 0;
            for (const auto& set : doc["data"]["database"]["sets"]) {
                if (!set.contains("translation-units")) continue;
                for (const auto& tu : set["translation-units"]) {
                    if (!tu.value("source", std::string()).contains(source_contains)) continue;
                    std::string one;
                    for (const auto& a : tu["arguments"]) one += (one.empty() ? "" : " ") + a.get<std::string>();
                    out += (units++ ? " " : "") + one;
                }
            }
            return std::pair{out, units};
        };
        static constexpr std::string_view realised[] = {"--target=x86_64-pc-cygwin", "-fno-short-wchar", "-D__MCPP_TARGET_WINDOWS__=1"};
        const auto [consumer, consumer_units] = joined("main.cpp");
        for (auto tok : realised)
            expect(fatal(e2e::contains(consumer, tok))) << "FAIL: ordinary package is missing realised token " << tok << "\n  args: " << consumer;
        const auto [libc, libc_units] = joined("fakemusl/src/lib.c");
        expect(fatal(e2e::contains(libc, "--target=x86_64-pc-cygwin"))) << "FAIL: the c-abi provider's own unit is missing the realised triple\n  args: " << libc;
        const auto [asm_joined, asm_units] = joined("probe.S");
        expect(fatal(asm_units > 0u)) << "FAIL: could not find the .S unit's compile command at all";
        for (auto tok : realised)
            expect(fatal(e2e::contains(asm_joined, tok))) << "FAIL: the assembly unit is missing realised token " << tok << " -- the [c-abi] substitution must reach .S the same as .c/.cpp\n  args: " << asm_joined;
        const auto [shim, shim_units] = joined("shim.c");
        expect(fatal(!shim.empty())) << "FAIL: could not find openkalwin's compiled unit at all";
        expect(fatal(s::lacks(shim, "--target=x86_64-pc-cygwin"))) << "FAIL: a kernel-abi provider must be INFERRED into the platform boundary and must not receive the realised triple, even though it declares no c-environment itself\n  args: " << shim;
        // B: a request the target cannot realise refuses the build.
        p.write("fakemusl/mcpp.toml", "[package]\nname     = \"fakemusl\"\nversion  = \"1.0.0\"\nprovides = [\"mcpp:c-abi=fakemusl\"]\n\n[targets.fakemusl]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n\n[c-abi]\npresents   = \"posix\"\ndata-model = \"llp64\"\nwchar      = 32\n");
        p.remove("target");
        r = p.mcpp(with({"build"}));
        const auto out = s::text(r);
        expect(fatal(!r)) << "FAIL: an unrealisable [c-abi] request must refuse the build: " << out;
        expect(fatal(e2e::contains(out, "x86_64-windows-gnu") || e2e::contains(out, "windows"))) << "FAIL: the refusal must name the target: " << out;
        expect(fatal(s::contains_nocase(out, "llp64"))) << "FAIL: the refusal must name the request: " << out;
    };

    "744 a declared absence explains the link that asked for it"_test[s::gcc] = [] {
        // A `[c-abi-absent]` row with `form = "link"` reaches the link: the linker's diagnostic
        // survives and the row's note follows it, naming the C library and calling the absence
        // a property of the environment.
        s::project p({{"libc/src/lib.c", "int fake_c_library_marker(void) { return 0; }\n"},
                      {"src/main.c", "extern int mcpp_absent_probe_fn(void);\n/* `_start`, not `main`: this links without startup files, so there is no C\n   runtime to call main. Nothing runs it -- the subject is the link. */\nvoid _start(void) { mcpp_absent_probe_fn(); }\n"},
                      {"mcpp.toml", "[package]\nname    = \"absence-explains-the-link\"\nversion = \"0.1.0\"\n\n[targets.absence-explains-the-link]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[dependencies]\nfakelibc = { path = \"libc\" }\n\n[build]\nallow_host_libs = true\nldflags = [\"-nostdlib\", \"-nostartfiles\", \"-static\"]\n"},
                      {"libc/mcpp.toml", "[package]\nname     = \"fakelibc\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:c-abi=musl\"]\n\n[targets.fakelibc]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n\n[c-abi-absent]\nmcpp_absent_probe_fn = { form = \"link\", note = \"this environment has no such operation\" }\n"}});
        auto r = p.mcpp({"build"});
        const auto out = s::text(r);
        expect(fatal(!r)) << "FAIL: the link must fail -- nothing defines mcpp_absent_probe_fn: " << out;
        expect(fatal(s::contains_nocase(out, "mcpp_absent_probe_fn"))) << "FAIL: the linker's own diagnostic must survive: " << out;
        expect(fatal(e2e::contains(out, "this environment has no such operation"))) << "FAIL: the [c-abi-absent] row's note must reach the link diagnostic: " << out;
        expect(fatal(e2e::contains(out, "the C library in this graph (musl) declares that it does not supply"))) << "FAIL: the note must name the C library in a closed parenthetical: " << out;
        expect(fatal(e2e::contains(out, "not a defect in the build"))) << "FAIL: the note must say an absence is a property of the environment: " << out;
        expect(fatal(s::lacks(out, "unsupported key"))) << "FAIL: the manifest this test builds has a key mcpp ignored, so the link above was not the one this test describes";
        expect(fatal(!s::has_line_nocase(out, "crt1|crti|multiple definition"))) << "FAIL: the link reached the host's C runtime: " << out;
    };

    "1014 host only layers do not select the target abi"_test[s::gcc][s::unix_shell] = [] {
        // A library build.mcpp uses may supply a host layer; neither it nor what reaches it
        // through another library selects the target's ABI, and the target's own instance of it
        // keeps its layer.
        const s::options o{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        s::project(tmp / "provider", {{"mcpp.toml", "[package]\nname = \"host-provider\"\nversion = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=openkal\"]\n[targets.host-provider]\nkind = \"lib\"\n"},
                                      {"src/provider.cppm", "export module host.provider;\nexport int host_probe(){return 7;}\n"}});
        s::project(tmp / "adapter", {{"mcpp.toml", "[package]\nname = \"host-adapter\"\nversion = \"0.1.0\"\n[dependencies]\nhost-provider = { path = \"../provider\" }\n[targets.host-adapter]\nkind = \"lib\"\n"},
                                     {"src/adapter.cppm", "export module host.adapter;\nimport host.provider;\nexport int adapter_probe(){return host_probe();}\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname = \"target-consumer\"\nversion = \"0.1.0\"\n[build-dependencies]\nhost-provider = { path = \"../provider\", host-module = true }\n"},
                                     {"build.mcpp", "import host.provider;\nint main(){return host_probe()==7?0:1;}\n"},
                                     {"src/main.cpp", "#ifdef __OPENKAL__\n#error host_kernel_layer_leaked_to_target\n#endif\nint main(){return 0;}\n"}});
        auto r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: direct host layer leaked: " << s::text(r);
        auto manifest = app.read("mcpp.toml");
        const std::string from = "host-provider = { path = \"../provider\"", to = "host-adapter = { path = \"../adapter\"";
        if (auto at = manifest.find(from); at != std::string::npos) manifest.replace(at, from.size(), to);
        app.write("mcpp.toml", manifest);
        app.write("build.mcpp", "import host.adapter;\nint main(){return adapter_probe()==7?0:1;}\n");
        r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: transitive host layer leaked: " << s::text(r);
        app.write("mcpp.toml", app.read("mcpp.toml") + "[dependencies]\nhost-provider = { path = \"../provider\" }\n");
        app.write("src/main.cpp", "#ifndef __OPENKAL__\n#error target_kernel_layer_was_withdrawn\n#endif\nint main(){return 0;}\n");
        r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: target instance lost its layer: " << s::text(r);
    };

    "1055 layer suppliers that require one another are refused"_test[s::gcc] = [] {
        // A kernel-abi supplier that requires the c-abi and a c-abi supplier that requires it are
        // a circle, refused with both suppliers named and a `layer-cycle` code; one direction is
        // no circle.
        const auto tmp = s::case_dir();
        s::project(tmp / "kimpl", {{"mcpp.toml", "[package]\nname     = \"kimpl\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=toykernel\"]\nrequires = [\"mcpp:c-abi\"]\n\n[targets.kimpl]\nkind = \"lib\"\n"},
                                   {"src/k.c", "int toy_kernel(void) { return 1; }\n"}});
        s::project clib(tmp / "clib", {{"mcpp.toml", "[package]\nname     = \"clib\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:c-abi=toylibc\"]\nrequires = [\"mcpp:kernel-abi=toykernel\"]\n\n[targets.clib]\nkind = \"lib\"\n"},
                                       {"src/c.c", "int toy_libc(void) { return 2; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nkimpl = { path = \"../kimpl\" }\nclib  = { path = \"../clib\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n"},
                                     {"src/main.c", "int main(void) { return 0; }\n"}});
        auto r = app.mcpp({"build", "--message-format", "json"});
        const auto out = s::text(r);
        expect(fatal(!r)) << "a circle of layer requirements built: " << out;
        expect(fatal(e2e::contains(out, "require one another in a circle"))) << "the refusal names no circle: " << out;
        expect(fatal(e2e::contains(out, "`kimpl@0.1.0` supplies the kernel-abi and requires the c-abi"))) << "the kernel interface's supplier is not named: " << out;
        expect(fatal(e2e::contains(out, "`clib@0.1.0` supplies the c-abi and requires the kernel-abi"))) << "the C library's supplier is not named: " << out;
        expect(fatal(e2e::contains(out, "[layer-cycle]"))) << "the refusal carries no layer-cycle code: " << out;
        auto manifest = clib.read("mcpp.toml");
        const std::string line = "requires = [\"mcpp:kernel-abi=toykernel\"]\n";
        if (auto at = manifest.find(line); at != std::string::npos) manifest.erase(at, line.size());
        clib.write("mcpp.toml", manifest);
        const auto again = s::text(app.mcpp({"build"}));
        expect(fatal(s::lacks(again, "require one another in a circle"))) << "a one-way requirement was refused: " << again;
    };
}};

}  // namespace
