// The openkal stack as packages: the older toolchain spelling, a kernel interface from the graph
// beside the payload's C library, the whole target side from packages natively and crossed to
// aarch64, one host reaching every openkal target, and FreeBSD's musl identity (formerly
// tests/e2e/269, 285, 286, 287, 289, 1073).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `file -b`'s "statically linked": no interpreter and no needed library.
bool statically_linked(const s::header& h) { return h.kind == s::format::elf && !h.elf_interpreter && h.needed.empty(); }

// The resolved driver of a build's report (`sed -n 's/.*Resolved [^ ]* → \(.*\)$/\1/p' | head -1`).
std::string resolved_driver(std::string_view out) {
    const std::regex row{R"(Resolved [^ ]* → (.*)$)"};
    std::istringstream in{std::string(out)};
    for (std::string line; std::getline(in, line);) {
        std::smatch m;
        if (std::regex_search(line, m, row)) return m[1];
    }
    return "";
}

// `case "$out" in *"<a>"*"<b>"*`: `a` occurs, and `b` after it, anywhere in the text.
bool in_order(std::string_view text, std::string_view a, std::string_view b) {
    const auto at = text.find(a);
    return at != std::string_view::npos && text.find(b, at + a.size()) != std::string_view::npos;
}

// The words a build cannot reach its packages with (the scripts' SKIP).
bool packages_unreachable(std::string_view out) {
    return out.contains("not found in the synced index") || out.contains("install_packages failed");
}

E2E stack{"stack", [] {
    "269 openkal llvm spelling still resolves"_test[s::import_std_libcxx] = [] {
        // `openkal-llvm@<v>` resolves to the driver `llvm@<v>` does, and decides nothing of the
        // target side: with an empty graph every layer is the payload's.
        const auto v = s::llvm_version();
        s::project p({{"src/main.cpp", "int main() { return 0; }\n"}});
        const auto manifest = [&](std::string_view spec) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"spelling-probe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}\"\n", spec));
        };
        const auto driver_for = [&](std::string_view spec) {
            manifest(spec);
            fs::remove_all(p.at("target"));
            return resolved_driver(s::text(p.mcpp({"build"})));
        };
        const auto now = driver_for("llvm@" + v);
        const auto old = driver_for("openkal-llvm@" + v);
        expect(fatal(!now.empty())) << "could not read the resolved driver for the current spelling";
        expect(fatal(now == old)) << "the two spellings must resolve to the same driver: llvm@" << v << " -> " << now << ", openkal-llvm@" << v << " -> " << old;

        manifest("openkal-llvm@" + v);
        fs::remove_all(p.at("target"));
        // MCPP_VERBOSE: an ordinary report prints only the layers the payload did not supply.
        const auto out = s::text(p.mcpp({"build"}, {.env = {{"MCPP_VERBOSE", "1"}}}));
        std::string report;
        for (const auto& l : s::lines_with(out, "kernel-abi")) report += l + "\n";
        for (const auto& l : s::lines_with(out, "c-abi"))
            if (!l.contains("kernel-abi")) report += l + "\n";
        expect(fatal(s::lacks(report, "graph"))) << "the toolchain family name must no longer decide where the target side comes from: " << report;
        expect(fatal(e2e::contains(report, "payload"))) << "with an empty graph every layer comes from the payload: " << report;
    };

    "285 kernel abi from graph keeps the payload c library"_test[s::gcc][s::unix_shell] = [] {
        // openkal-linux supplies the kernel interface and nothing below it: the C library, the
        // C++ runtime and the startup files stay the payload's, the link reaches them, and the
        // program (a throw and a std::string) runs.
        s::project p({{"mcpp.toml", "[package]\nname    = \"kabi\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n\n[dependencies]\nopenkal-linux = \"0.5.4\"\n"},
                      {"src/main.cpp", R"(#include <string>
#include <stdexcept>
#include <cstdio>

int main() {
    try { throw std::runtime_error("boom"); }
    catch (const std::exception& e) { std::printf("%s\n", e.what()); }
    std::string s = "x"; s += "y";
    return s == "xy" ? 0 : 1;
}
)"}});
        auto r = p.mcpp({"build"});
        const auto out = s::text(r);
        // The script's shape guard printed SKIP and exited 0; it is a skip here.
        if (!in_order(out, "kernel-abi", "graph")) skip("the graph did not supply the kernel interface here");
        if (in_order(out, "c-abi", "graph")) skip("the C library came from the graph too — not the shape under test");
        expect(fatal(e2e::succeeded(r))) << "the build failed with the C library on the payload: " << out;
        const auto bin = s::find_file(p.at("target"), "kabi");
        expect(fatal(bin.has_value())) << "no artefact was produced";
        auto ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "the artefact does not run: " << s::text(ran);
        expect(fatal(s::text(ran) == "boom\n")) << "wrong output: " << s::text(ran);
    };

    "286 the openkal stack still builds"_test[s::llvm][s::unix_shell] = [] {
        // openkal-musl and openkal-llvm-runtime supply every layer: the report says so by
        // layer, the program is static with no interpreter and runs; the hosted threads
        // companion (nlohmann.json, threads, TLS, concurrent unwind) runs too.
        s::project p({{"mcpp.toml", std::format("[package]\nname    = \"okstack\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies]\nopenkal-musl = \"0.3.5\"\nopenkal-llvm-runtime = \"0.1.3\"\nnlohmann.json = \"3.12.0\"\n", s::llvm_version())},
                      {"src/main.cpp", R"(#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> v;
    for (int i = 0; i < 4; ++i) v.push_back("x" + std::to_string(i));
    std::string joined;
    for (auto const& s : v) joined += s;
    std::printf("%s %zu\n", joined.c_str(), v.size());
    return joined == "x0x1x2x3" && v.size() == 4 ? 0 : 1;
}
)"}});
        if (auto index = s::env("MCPP_OPENKAL_INDEX"); index && !index->empty()) {
            const auto root = fs::weakly_canonical(*index);
            expect(fatal(fs::is_regular_file(root / "pkgs/n/nlohmann.json.lua"))) << root;
            const auto relative = fs::relative(root, fs::weakly_canonical(p.dir())).generic_string();
            p.write("mcpp.toml", p.read("mcpp.toml") + "\n[indices]\nnlohmann = { path = " + nlohmann::json(relative).dump() + " }\n");
        }
        auto r = p.mcpp({"build"});
        auto out = s::text(r);
        if (!e2e::succeeded(r) && packages_unreachable(out)) skip("the openkal packages are not reachable from here");
        expect(fatal(e2e::succeeded(r))) << "the openkal stack did not build: " << out;
        for (std::string_view layer : {"kernel-abi", "c-abi", "c\\+\\+-abi"})
            expect(s::has_line(out, std::format("{}.*graph", layer))) << layer << " did not come from the graph: " << out;
        auto bin = s::find_file(p.at("target"), "okstack");
        expect(fatal(bin.has_value())) << "no artefact";
        const auto h = s::read_header(*bin);
        expect(fatal(h.kind == s::format::elf)) << "the artefact was not read as ELF: " << *bin;
        if (s::env("MCPP_E2E_EXPECT_ARCH") == "aarch64")
            expect(fatal(h.wide && h.machine == s::elf_machine::aarch64)) << "native ARM64 job produced a different architecture: machine " << h.machine;
        expect(fatal(statically_linked(h))) << "not static — the payload's C library was linked instead (interpreter '" << h.interpreter << "')";
        expect(fatal(!h.elf_interpreter)) << "the image names an interpreter: " << h.interpreter;
        auto ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "the artefact does not run: " << s::text(ran);
        expect(fatal(s::text(ran) == "x0x1x2x3 4\n")) << "wrong output: " << s::text(ran);

        // The resolved native stack and its cache, for hosted thread/TLS execution.
        p.write("src/main.cpp", s::read_file(s::repository() / "tests/fixtures/openkal-hosted-threads/src/main.cpp.in"));
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the hosted threads companion did not build: " << s::text(r);
        bin = s::find_file(p.at("target"), "okstack");
        expect(fatal(bin.has_value())) << "no hosted threads artefact";
        ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "the hosted threads companion did not run: " << s::text(ran);
        expect(fatal(s::text(ran) == "openkal indexed JSON: dump, parse, literals and ordered_json ok\nopenkal hosted threads: isolation, destructors and concurrent unwind ok\n"))
            << "wrong hosted threads output: " << s::text(ran);
    };

    "287 the openkal stack crosses to aarch64"_test[s::llvm][s::unix_shell] = [] {
        // aarch64-linux-musl over openkal: an aarch64 static ELF whose outline-atomics helpers
        // are defined (none undefined), with LSE instructions in it (the feature on, not off);
        // where qemu-aarch64 exists it runs.
        s::project p({{"mcpp.toml", std::format("[package]\nname    = \"okcross\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies]\nopenkal-musl = \"0.3.5\"\nopenkal-llvm-runtime = \"0.1.3\"\n", s::llvm_version())},
                      {"src/main.cpp", R"(#include <atomic>
#include <cstdio>

int main() {
    std::atomic<int> a{7};
    int expected = 7;
    const bool swapped = a.compare_exchange_strong(expected, 42);
    const int before = a.fetch_add(5);
    std::printf("%d %d %d\n", swapped ? 1 : 0, before, a.load());
    return (swapped && before == 42 && a.load() == 47) ? 0 : 1;
}
)"}});
        auto r = p.mcpp({"build", "--target", "aarch64-linux-musl"});
        const auto out = s::text(r);
        if (!e2e::succeeded(r) && packages_unreachable(out)) skip("the openkal packages are not reachable from here");
        if (!e2e::succeeded(r) && out.contains("cannot be built on this host")) skip("aarch64-linux-musl cannot be built here");
        expect(fatal(e2e::succeeded(r))) << "the cross build failed: " << out;
        const auto bin = s::find_file(p.at("target"), "okcross");
        expect(fatal(bin.has_value())) << "no artefact";
        const auto h = s::read_header(*bin);
        expect(fatal(h.kind == s::format::elf && h.machine == s::elf_machine::aarch64)) << "wrong machine: " << h.machine;
        expect(fatal(statically_linked(h))) << "not static (interpreter '" << h.interpreter << "')";

        // The symbol table, read here (the script's `nm`): defined and undefined helpers.
        std::size_t defined = 0, undefined = 0;
        const auto defs = s::elf_symbols(*bin, ".symtab", true);
        for (const auto& n : defs)
            if (n.starts_with("__aarch64_")) ++defined;
        for (const auto& n : s::elf_symbols(*bin))
            if (n.starts_with("__aarch64_") && !std::ranges::contains(defs, n)) ++undefined;
        expect(fatal(defined > 0 && undefined == 0)) << defined << " defined, " << undefined << " undefined — the graph did not supply them";

        // The disassembler of the toolchain that built it (it reads every target clang emits),
        // else PATH's llvm-objdump, else objdump.
        std::optional<fs::path> objdump;
        for (const auto& c : s::glob(s::shared_home() / "registry/data/xpkgs/xim-x-llvm", "*/bin/llvm-objdump")) objdump = c;
        if (!objdump) objdump = s::on_path("llvm-objdump");
        if (!objdump) objdump = s::on_path("objdump");
        if (objdump) {
            const auto disasm = s::run(*objdump, {"-d", bin->string()}).out;
            const auto insns = s::count_matching(disasm, R"(^\s*[0-9a-f]+:)");
            // Zero instructions: the tool could not read the file, which says nothing about LSE.
            if (insns > 0) {
                const auto lse = s::count_matching(disasm, R"(\b(casal|cas|ldaddal|ldadd|swpal|swp)\b)");
                expect(fatal(lse > 0)) << insns << " instructions disassembled and not one LSE; `+outline-atomics` is disabled";
            }
        }
        auto runner = s::on_path("qemu-aarch64");
        if (!runner) runner = s::on_path("qemu-aarch64-static");
        if (runner) {
            auto ran = s::run(*runner, {bin->string()});
            expect(fatal(e2e::succeeded(ran))) << "non-zero exit under " << runner->filename() << ": " << s::text(ran);
            expect(fatal(s::text(ran) == "1 42 47\n")) << "wrong output under emulation: " << s::text(ran);
        }
    };

    "289 one host reaches every openkal target"_test[s::llvm][s::unix_shell] = [] {
        // One source per kind, one host, every target the ecosystem serves: each row builds the
        // artefact its machine names, or is skipped with its reason; a sweep that built nothing
        // is a skip, and a row that fails fails the case.
        struct row {
            std::string target, deps;
            s::format kind;
            std::uint32_t machine;  // 0: a PE32+ of any machine
            bool static_link;
        };
        const std::string musl_runtime = "openkal-musl = \"0.3.5\"\nopenkal-llvm-runtime = \"0.1.3\"\n";
        const std::array rows{
            row{"x86_64-linux-musl", musl_runtime, s::format::elf, s::elf_machine::x86_64, true},
            row{"aarch64-linux-musl", musl_runtime, s::format::elf, s::elf_machine::aarch64, true},
            row{"x86_64-windows-gnu", musl_runtime + "openkal-windows = \"0.1.5\"\n", s::format::pe, 0, false},
            row{"riscv64-none-elf", "openkal = \"0.7.0\"\nopenkal-opensbi = { version = \"0.1.5\", features = [\"standalone\"] }\n", s::format::elf, s::elf_machine::riscv, true},
        };
        int built = 0, skipped = 0;
        std::string failures;
        for (const auto& w : rows) {
            const bool bare = w.target.contains("-none-");
            // `sysroot = ""` is how a project says "no C library"; the target does not imply it.
            const auto manifest = std::format("[package]\nname    = \"sweep\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n{}[dependencies]\n{}\n",
                                              s::llvm_version(), bare ? std::format("[target.{}]\nsysroot = \"\"\n\n", w.target) : "", w.deps);
            const auto source = bare ? R"(import openkal.stream;
extern "C" int main(int, char**, char**) {
    const char m[] = "sweep\n";
    kal_stream_write(kal_stdout(), m, sizeof m - 1);
    return 0;
}
)" : R"(#include <cstdio>
#include <string>
int main() {
    std::string s = "sweep";
    std::printf("%s\n", s.c_str());
    return s.size() == 5 ? 0 : 1;
}
)";
            s::project p(s::case_dir() / w.target, {{"mcpp.toml", manifest}, {"src/main.cpp", source}});
            auto r = p.mcpp({"build", "--target", w.target});
            const auto out = s::text(r);
            if (!e2e::succeeded(r)) {
                if (packages_unreachable(out) || out.contains("cannot be built on this host")) { ++skipped; continue; }
                failures += std::format("{} — the build failed:\n{}\n", w.target, out);
                continue;
            }
            auto bin = s::find_file(p.at("target"), "sweep");
            if (!bin) bin = s::find_file(p.at("target"), "sweep.exe");
            if (!bin) { failures += std::format("{} — the build reported success and produced nothing\n", w.target); continue; }
            const auto h = s::read_header(*bin);
            bool ok = h.kind == w.kind && h.wide;
            if (w.kind == s::format::elf) ok = ok && h.machine == w.machine && h.elf_type == 2;  // ET_EXEC: "LSB executable"
            if (w.static_link) ok = ok && statically_linked(h);
            if (ok) ++built;
            else failures += std::format("{} — wrong artefact: kind {}, machine {}, type {}, interpreter '{}'\n", w.target, int(h.kind), h.machine, h.elf_type, h.interpreter);
        }
        expect(fatal(failures.empty())) << failures;
        if (built == 0) skip(std::format("no target could be built here ({} skipped)", skipped));
    };

    "1073 the freebsd musl identity is served by openkal"_test[s::llvm][s::elf] = [] {
        // `--target x86_64-freebsd-musl` builds a static FreeBSD ELF over openkal-musl and
        // openkal-freebsd; `--target x86_64-freebsd` with no platform selected takes that
        // identity and says so.
        auto app = s::project::scaffold("app");
        auto r = app.mcpp({"build", "--target", "x86_64-freebsd-musl"});
        expect(fatal(e2e::succeeded(r))) << "A: x86_64-freebsd-musl did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "openkal-musl@"))) << "A: the C library is not openkal-musl: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "openkal-freebsd@"))) << "A: the kernel layer is not openkal-freebsd: " << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::glob(app.at("target/x86_64-freebsd-musl"), "*/bin/app"))
            if (fs::is_regular_file(f)) exe = f;
        expect(fatal(exe.has_value())) << "A: no program";
        // `file`'s "FreeBSD": the ELF's OS/ABI byte (EI_OSABI 9) or a FreeBSD note.
        const auto bytes = s::read_file(*exe);
        const bool freebsd = (bytes.size() > 7 && bytes[7] == 9) || bytes.contains(std::string_view("FreeBSD\0", 8));
        expect(fatal(freebsd)) << "A: not a FreeBSD program";
        expect(fatal(statically_linked(s::read_header(*exe)))) << "A: not static";
        r = app.mcpp({"build", "--target", "x86_64-freebsd"});
        expect(fatal(e2e::succeeded(r))) << "B: x86_64-freebsd with no platform did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "x86_64-freebsd-musl"))) << "B: the request does not say it took the musl identity: " << s::text(r);
    };
}};

}  // namespace
