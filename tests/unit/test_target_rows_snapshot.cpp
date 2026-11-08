
import std;
import mcpplibs.mtest;
import mcpp.toolchain.triple;
import mcpp.toolchain.platform;
import mcpp.freestanding.target;
import mcpp.freestanding.linkline;

using namespace mcpplibs::mtest;

// THE EQUIVALENCE GATE OF THE TARGET MODEL (design 2026-11-09 §9, §14).
//
// The target rows moved from code to data. Every fact a row used to answer in
// code is rendered here, one line per fact, and compared with
// tests/fixtures/target-rows.snapshot, which was written by the code before the
// move. A difference is a change of behaviour of a built-in target; it is either
// a defect of the data or a deliberate change, and a deliberate change rewrites
// the snapshot (MCPP_SNAPSHOT_UPDATE=1) in the same commit that explains it.

namespace tr = mcpp::toolchain::triple;
namespace fs = mcpp::freestanding;

namespace {

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (auto const& s : v) { if (!out.empty()) out += ' '; out += s; }
    return out;
}

std::string yes(bool b) { return b ? "1" : "0"; }

std::string facts_of(const tr::Triple& t) {
    std::string o;
    auto line = [&](std::string_view k, std::string v) { o += std::format("  {} {}\n", k, v); };
    line("str", t.str());
    line("llvm", t.llvm_triple());
    line("llvm@15.0", t.llvm_triple("15.0"));
    line("format", std::string(tr::to_string(t.object_format())));
    line("family", t.family());
    line("pe/macho/wasm", yes(t.is_pe()) + yes(t.is_mach_o()) + yes(t.is_wasm()));
    line("apple/ios/sim/android", yes(t.is_apple()) + yes(t.is_ios()) + yes(t.is_ios_simulator())
                                      + yes(t.is_android()));
    line("freestanding/openkal/own-sysroot/capability",
         yes(t.is_freestanding()) + yes(t.is_openkal()) + yes(t.has_own_sysroot())
             + yes(t.pin_is_capability()));
    line("musl/msvc/windows-gnu/x86_32", yes(t.is_musl()) + yes(t.is_msvc_env())
                                             + yes(t.is_windows_gnu()) + yes(t.is_x86_32()));
    line("msvc-arch", std::string(t.msvc_arch()));
    line("nasm", t.nasm_format().value_or("-"));
    const tr::ArtifactNaming host{".host", "hostlib", ".hosta", ".hostso", false};
    auto n = tr::artifact_naming(t, host);
    line("naming", std::format("exe={} lib={} static={} shared={} import={} sharedPrefix={}",
                               n.exeSuffix, n.libPrefix, n.staticLibExt, n.sharedLibExt,
                               yes(n.sharedNeedsImportLib),
                               n.sharedLibPrefixSet ? std::string(n.sharedLibPrefix) : "-"));
    line("application", tr::application_form(t) == tr::ApplicationForm::Executable ? "exe" : "shared");
    line("android-abi", tr::android_abi(t));
    line("platform-name", tr::platform_name(t));
    line("descriptor-keys", join(mcpp::toolchain::platform::descriptor_keys_for_os(t.os)));
    line("sysroot", tr::effective_sysroot(t, nullptr));
    if (auto s = fs::resolve(t)) {
        line("fs.spec", std::format("march={} mabi={} mcmodel={} libdir={} lld={}", s->march, s->mabi,
                                    s->mcmodel, s->libdir, s->lldEmulation));
        line("fs.compile", join(fs::compile_flags(*s, false)));
        line("fs.compile+rt", join(fs::compile_flags(*s, true)));
        auto esc = [](const std::filesystem::path& p) { return p.generic_string(); };
        fs::LinkInputs in{ "/lld", "/script.ld", "/sysroot/lib", {}, {} };
        line("fs.link", fs::link_flags(*s, in, esc));
        if (!s->lldEmulation.empty()) line("fs.link-direct", fs::link_flags_direct(*s, in, esc));
    }
    return o;
}

std::string render() {
    std::string o;
    for (auto const& k : tr::known_targets()) {
        o += std::format("row {}\n", k.canonical);
        o += std::format("  table tier={} note={} pin={} sysroot={} static={} platform={}\n", k.tier,
                         k.note, k.pin, k.sysroot, yes(k.defaultStatic), k.platform);
        if (auto t = tr::parse(k.canonical)) o += facts_of(*t);
        else o += "  unparseable\n";
    }
    // The spellings a request may take, and what each resolves to before any
    // provider is consulted.
    static constexpr std::string_view kRequests[] = {
        "x86_64-linux", "aarch64-linux", "riscv64-linux", "x86_64-windows", "i686-windows",
        "aarch64-windows", "x86_64-macos", "aarch64-macos", "aarch64-ios", "x86_64-openkal",
        "x86_64-w64-mingw32", "x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc",
        "arm64-apple-darwin24.1.0", "arm64-apple-macosx15.0", "aarch64-unknown-linux-musl",
        "aarch64-linux-android21", "wasm32-unknown-emscripten", "x86-windows-msvc",
        "i686-none-elf", "x86_64-freebsd", "x86_64-linux-gnux32",
    };
    for (auto r : kRequests) {
        o += std::format("request {}\n", r);
        auto t = tr::parse(r);
        if (!t) { o += "  unparseable\n"; continue; }
        auto res = tr::resolve_request(*t);
        std::vector<std::string> sib(res.siblings.begin(), res.siblings.end());
        std::vector<std::string> sup(res.supported.begin(), res.supported.end());
        o += std::format("  parsed {} envExplicit={}\n", t->str(), yes(t->envExplicit));
        o += std::format("  resolved {} completed={} ambiguous={}\n", res.triple.str(),
                         yes(res.completedFromVocabulary), yes(res.ambiguous));
        o += std::format("  siblings {}\n  supported {}\n", join(sib), join(sup));
        o += std::format("  known {}\n", yes(tr::is_known_target(res.triple)));
    }
    static constexpr std::string_view kTypos[] = {
        "x86_64-linux-mus", "x86-64-linux-gnu", "aarch64-macso", "riscv64-none-eld", "thumbv7m-none",
    };
    for (auto r : kTypos) o += std::format("did-you-mean {} {}\n", r, tr::did_you_mean(r).value_or("-"));
    namespace ts = mcpp::toolchain::target_set;
    auto platformOf = [](const tr::Triple& t) -> std::expected<std::string, std::string> {
        if (auto* k = tr::find_known_target(t)) return std::string(k->platform);
        return std::string{};
    };
    static constexpr std::string_view kSets[] = {
        "linux", "linux-musl", "windows", "windows-msvc", "macos", "openkal", "none-elf", "none-eabihf",
        "android", "ios", "ios-sim", "emscripten", "none",
    };
    for (auto s : kSets) {
        auto e = ts::expand_set(s, platformOf);
        if (!e) { o += std::format("set {} refused\n", s); continue; }
        o += std::format("set {} -> {} | {}\n", s, join(e->members), e->platform);
    }
    return o;
}

std::optional<std::filesystem::path> snapshot_path() {
    for (auto const* base : { "tests/fixtures/target-rows.snapshot", "../tests/fixtures/target-rows.snapshot",
                              "../../tests/fixtures/target-rows.snapshot" }) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(base).parent_path(), ec))
            return std::filesystem::path(base);
    }
    return std::nullopt;
}

}  // namespace

UTest every_built_in_row_answers_as_the_snapshot_records{"TargetRows/EveryBuiltInRowAnswersAsTheSnapshotRecords", [] {
    auto path = snapshot_path();
    expect(fatal(bool(path))) << "tests/fixtures not found from " << std::filesystem::current_path();
    const auto now = render();
    if (const char* u = std::getenv("MCPP_SNAPSHOT_UPDATE"); u && std::string_view(u) == "1") {
        std::ofstream(*path, std::ios::binary) << now;
        skip([&] { std::ostringstream text; text << "snapshot written to " << path->generic_string(); return text.str(); }());
    }
    std::ifstream in(*path, std::ios::binary);
    if (auto&& failed = expect(fatal(bool(in))); !failed) failed << "no snapshot at " << path->generic_string();
    const std::string was{std::istreambuf_iterator<char>(in), {}};
    if (now == was) return;
    // The first differing line, with its row, so the failure names the fact.
    std::istringstream a(was), b(now);
    std::string la, lb, row;
    for (int n = 1;; ++n) {
        const bool ga = bool(std::getline(a, la)), gb = bool(std::getline(b, lb));
        if (!ga && !gb) break;
        if (gb && (lb.starts_with("row ") || lb.starts_with("request "))) row = lb;
        if (la != lb || ga != gb)
            fail([&] { std::ostringstream text; text << "line " << n << " (" << row << ")\n  snapshot: " << (ga ? la : "<end>") << "\n  now:      " << (gb ? lb : "<end>"); return text.str(); }());
    }
}};
