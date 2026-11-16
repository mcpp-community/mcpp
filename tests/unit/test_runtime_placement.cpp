// mcpp.build.runtime_placement and the PE version readers it relies on.
//
// Runs on every host: the PE images are synthesised here, byte by byte, and
// the resolver reads versions through an injected function, so every
// combination of kinds, versions and contracts is a unit test rather than a
// Windows runner's.

import std;
import mcpplibs.mtest;
import mcpp.pack.binfmt;
import mcpp.build.runtime_placement;

using namespace mcpplibs::mtest;

namespace fs = std::filesystem;
namespace rp = mcpp::build::runtime_placement;
using mcpp::pack::binfmt::PeVersion;

namespace {

// ── a minimal PE32+ image with an optional RT_VERSION resource ─────────────

void put16(std::string& b, std::size_t at, std::uint16_t v) {
    if (b.size() < at + 2) b.resize(at + 2, '\0');
    b[at] = static_cast<char>(v & 0xFF);
    b[at + 1] = static_cast<char>(v >> 8);
}
void put32(std::string& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4, '\0');
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// One section, `.rsrc`, at RVA 0x1000 and file offset 0x400. With a version,
// the section holds a three-level resource tree (type 16, name 1, language
// 1033) whose data entry points at a VS_VERSIONINFO carrying it.
std::string make_pe(std::optional<PeVersion> fileVersion, std::uint8_t linkerMajor,
                    std::uint8_t linkerMinor) {
    std::string b(0x400, '\0');
    b[0] = 'M'; b[1] = 'Z';
    put32(b, 0x3C, 0x80);                        // e_lfanew
    const std::size_t nt = 0x80;
    b[nt] = 'P'; b[nt + 1] = 'E';
    put16(b, nt + 4, 0x8664);                    // machine x64
    put16(b, nt + 6, 1);                         // one section
    put16(b, nt + 20, 240);                      // SizeOfOptionalHeader (PE32+, 16 dirs)
    const std::size_t opt = nt + 24;
    put16(b, opt, 0x20b);                        // PE32+
    b[opt + 2] = static_cast<char>(linkerMajor);
    b[opt + 3] = static_cast<char>(linkerMinor);
    put32(b, opt + 108, 16);                     // NumberOfRvaAndSizes
    const std::size_t dirs = opt + 112;
    const std::uint32_t rsrcRva = 0x1000, rsrcRaw = 0x400;
    // Section table: `.rsrc`, VirtualSize, VA, SizeOfRawData, PointerToRawData.
    const std::size_t sec = opt + 240;
    std::memcpy(b.data() + sec, ".rsrc\0\0\0", 8);
    put32(b, sec + 8, 0x200);
    put32(b, sec + 12, rsrcRva);
    put32(b, sec + 16, 0x200);
    put32(b, sec + 20, rsrcRaw);
    b.resize(rsrcRaw + 0x200, '\0');
    if (!fileVersion) return b;

    put32(b, dirs + 2 * 8, rsrcRva);             // directory 2: resources
    put32(b, dirs + 2 * 8 + 4, 0x200);
    // Level 1 at +0x00: one id entry, type 16, subdirectory at +0x18.
    const std::size_t r = rsrcRaw;
    put16(b, r + 14, 1);
    put32(b, r + 16, 16);
    put32(b, r + 20, 0x80000000u | 0x18);
    // Level 2 at +0x18: one id entry, name 1, subdirectory at +0x30.
    put16(b, r + 0x18 + 14, 1);
    put32(b, r + 0x18 + 16, 1);
    put32(b, r + 0x18 + 20, 0x80000000u | 0x30);
    // Level 3 at +0x30: one id entry, language 1033, data entry at +0x48.
    put16(b, r + 0x30 + 14, 1);
    put32(b, r + 0x30 + 16, 1033);
    put32(b, r + 0x30 + 20, 0x48);
    // Data entry at +0x48: RVA of the block, and its size.
    const std::uint32_t blockOff = 0x60;
    put32(b, r + 0x48, rsrcRva + blockOff);
    put32(b, r + 0x48 + 4, 92);
    // VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO\0",
    // padding to 32 bits, then VS_FIXEDFILEINFO.
    const std::size_t v = r + blockOff;
    put16(b, v, 92);
    put16(b, v + 2, 52);
    put16(b, v + 4, 0);
    const std::u16string key = u"VS_VERSION_INFO";
    for (std::size_t i = 0; i < key.size(); ++i) put16(b, v + 6 + i * 2, key[i]);
    const std::size_t fixed = v + 40;
    put32(b, fixed, 0xFEEF04BDu);
    put32(b, fixed + 4, 0x00010000u);
    put32(b, fixed + 8, (std::uint32_t(fileVersion->major) << 16) | fileVersion->minor);
    put32(b, fixed + 12, (std::uint32_t(fileVersion->build) << 16) | fileVersion->revision);
    return b;
}

}  // namespace

UTest reads_the_fixed_file_version_and_the_linker_version{"PeVersionReader/ReadsTheFixedFileVersionAndTheLinkerVersion", [] {
    const auto image = make_pe(PeVersion{14, 44, 35112, 1}, 14, 44);
    auto v = mcpp::pack::binfmt::pe_file_version(std::string_view{image});
    expect(fatal(v.has_value()));
    expect(eq(*v, (PeVersion{14, 44, 35112, 1})));
    expect(eq(v->str(), "14.44.35112.1"));
    auto l = mcpp::pack::binfmt::pe_linker_version(std::string_view{image});
    expect(fatal(l.has_value()));
    expect(eq(l->major, 14));
    expect(eq(l->minor, 44));
}};

// A reader that cannot answer does not decide: no resource section, a
// truncated image and a file that is not PE all read as "unknown".
UTest an_image_without_a_version_reads_as_unknown{"PeVersionReader/AnImageWithoutAVersionReadsAsUnknown", [] {
    const auto noResource = make_pe(std::nullopt, 14, 0);
    expect(!mcpp::pack::binfmt::pe_file_version(std::string_view{noResource}));
    // The linker field is still there: lld-link writes 14.0.
    auto l = mcpp::pack::binfmt::pe_linker_version(std::string_view{noResource});
    expect(fatal(l.has_value()));
    expect(eq(l->minor, 0));

    auto truncated = make_pe(PeVersion{14, 44, 1, 0}, 14, 44);
    truncated.resize(0x420);
    expect(!mcpp::pack::binfmt::pe_file_version(std::string_view{truncated}));
    expect(!mcpp::pack::binfmt::pe_file_version(std::string_view{"\x7f" "ELF not a PE"}));
    expect(!mcpp::pack::binfmt::pe_linker_version(std::string_view{"MZ"}));
}};

UTest reads_a_version_from_a_file{"PeVersionReader/ReadsAVersionFromAFile", [] {
    const auto dir = fs::temp_directory_path()
        / std::format("mcpp-pever-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "vcruntime140.dll", std::ios::binary);
        const auto image = make_pe(PeVersion{14, 51, 36231, 0}, 14, 51);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
    }
    auto v = mcpp::pack::binfmt::pe_file_version(dir / "vcruntime140.dll");
    expect(fatal(v.has_value()));
    expect(eq(v->str(), "14.51.36231.0"));
    expect(!mcpp::pack::binfmt::pe_file_version(dir / "missing.dll"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}};

// ── the resolver ───────────────────────────────────────────────────────────

namespace {

const std::vector<std::string> kSet = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"};
const PeVersion kOld{14, 29, 30139, 0};
const PeVersion kToolset{14, 44, 35112, 1};
const PeVersion kNew{14, 51, 36231, 0};

enum class Ver { Old, Equal, New, Unreadable };
std::optional<PeVersion> version_of(Ver v) {
    switch (v) {
        case Ver::Old:        return kOld;
        case Ver::Equal:      return kToolset;
        case Ver::New:        return kNew;
        case Ver::Unreadable: return std::nullopt;
    }
    return std::nullopt;
}

struct Case {
    rp::CrtPolicy policy;
    std::optional<Ver> declared;       // a declared vcruntime140.dll, and its version
    bool toolset;                      // the toolset's set is a candidate
    std::optional<Ver> derived;        // a dependency directory's set, and its version
    bool derivedComplete;              // that set has every name of the toolset's
};

std::string describe(const Case& c) {
    auto v = [](std::optional<Ver> x) -> std::string {
        if (!x) return "none";
        switch (*x) {
            case Ver::Old: return "old"; case Ver::Equal: return "equal";
            case Ver::New: return "new"; case Ver::Unreadable: return "unreadable";
        }
        return "?";
    };
    const char* p = c.policy == rp::CrtPolicy::Carry ? "carry"
                  : c.policy == rp::CrtPolicy::System ? "system"
                  : c.policy == rp::CrtPolicy::Static ? "static" : "n/a";
    return std::format("policy={} declared={} toolset={} derived={}{}", p, v(c.declared),
                       c.toolset, v(c.derived), c.derivedComplete ? "" : " (incomplete)");
}

rp::Input input_for(const Case& c, std::map<fs::path, std::optional<PeVersion>>& versions) {
    rp::Input in;
    in.crt = c.policy;
    const fs::path toolsetDir = "/vs/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT";
    const fs::path qtBin = "/store/xim-x-qt-base/6.11.1/bin";
    // An ordinary DLL beside the derived runtime, which is never a CRT name.
    in.candidates.push_back({{qtBin / "Qt6Core.dll"}, "bin/Qt6Core.dll", rp::Kind::Derived});
    if (c.declared) {
        const fs::path p = "/project/vendor/vcruntime140.dll";
        versions[p] = version_of(*c.declared);
        in.candidates.push_back({{p}, "bin/vcruntime140.dll", rp::Kind::Declared});
    }
    if (c.toolset)
        for (auto const& n : kSet) {
            versions[toolsetDir / n] = kToolset;
            in.candidates.push_back({{toolsetDir / n}, fs::path("bin") / n, rp::Kind::Toolchain});
        }
    if (c.derived) {
        auto names = kSet;
        if (!c.derivedComplete) names.pop_back();
        for (auto const& n : names) {
            // MSVC-linked images spell the runtime in upper case; a directory
            // listing of Qt's bin/ does too on some installs.
            const auto spelled = n == "msvcp140.dll" ? std::string("MSVCP140.dll") : n;
            versions[qtBin / spelled] = version_of(*c.derived);
            in.candidates.push_back({{qtBin / spelled}, fs::path("bin") / spelled, rp::Kind::Derived});
        }
    }
    in.versionOf = [&versions](const fs::path& p) -> std::optional<PeVersion> {
        auto it = versions.find(p);
        return it == versions.end() ? std::nullopt : it->second;
    };
    return in;
}

}  // namespace

// Every combination of the contract's rule, a declared runtime file, the
// toolset's set and a dependency's set, with every version relation. The
// properties are the rule of mcpp.build.runtime_placement, stated once each.
UTest every_combination_of_kinds_versions_and_contracts{"RuntimePlacement/EveryCombinationOfKindsVersionsAndContracts", [] {
    int cases = 0;
    for (auto policy : {rp::CrtPolicy::Carry, rp::CrtPolicy::System, rp::CrtPolicy::Static,
                        rp::CrtPolicy::NotApplicable})
    for (std::optional<Ver> declared : {std::optional<Ver>{}, std::optional(Ver::Old),
                                         std::optional(Ver::Equal), std::optional(Ver::New),
                                         std::optional(Ver::Unreadable)})
    for (bool toolset : {true, false})
    for (std::optional<Ver> derived : {std::optional<Ver>{}, std::optional(Ver::Old),
                                        std::optional(Ver::Equal), std::optional(Ver::New),
                                        std::optional(Ver::Unreadable)})
    for (bool complete : {true, false}) {
        // A carrying contract always has the toolset's set: an explicit
        // toolchain-coupled without one is refused before the resolver runs.
        if (policy == rp::CrtPolicy::Carry && !toolset) continue;
        if (policy == rp::CrtPolicy::System && toolset) continue;   // not listed under host-coupled
        if (policy == rp::CrtPolicy::NotApplicable && toolset) continue;
        if (!derived && !complete) continue;
        const Case c{policy, declared, toolset, derived, complete};
        std::map<fs::path, std::optional<PeVersion>> versions;
        const auto in = input_for(c, versions);
        const auto d = rp::resolve(in);
        ++cases;
        /* trace: describe(c) */;

        // P1. One file per destination, compared without case.
        std::set<std::string> dests;
        for (auto const& p : d.placed)
            expect(bool(dests.insert(rp::fold(p.dest.generic_string())).second))
                << "two placements for " << p.dest.string();

        // P2. An ordinary name is never touched by the runtime rule.
        expect(dests.contains("bin/qt6core.dll"));

        std::vector<const rp::Placement*> crt;
        for (auto const& p : d.placed)
            if (rp::is_msvc_crt_name(p.dest.filename().string())) crt.push_back(&p);

        if (policy == rp::CrtPolicy::System) {
            // P3. Host-coupled: no copy of the runtime from any source; a
            // declared one is refused; a dependency's is stated.
            expect(crt.empty());
            expect(eq(!d.errors.empty(), declared.has_value()));
            if (derived) expect(!d.notes.empty());
            continue;
        }
        expect(d.errors.empty());
        if (policy == rp::CrtPolicy::NotApplicable) {
            // P4. Off the MSVC ABI the names take the kind order: a
            // declaration, else the derived file.
            if (declared) {
                auto it = std::ranges::find_if(d.placed, [](auto const& p) {
                    return rp::fold(p.dest.filename().string()) == "vcruntime140.dll"; });
                expect(fatal(ne(it, d.placed.end())));
                expect(eq(it->kind, rp::Kind::Declared));
            }
            continue;
        }

        // P5. A declared runtime file is always placed, as declared.
        if (declared) {
            auto it = std::ranges::find_if(crt, [](auto const* p) {
                return rp::fold(p->dest.filename().string()) == "vcruntime140.dll"; });
            expect(fatal(ne(it, crt.end())));
            expect(eq((*it)->kind, rp::Kind::Declared));
            // P6. D2: older than the toolset's runtime is a warning; an
            // unreadable version is a note; neither is said otherwise.
            const bool older = toolset && *declared == Ver::Old;
            expect(eq(!d.warnings.empty(), older));
        } else {
            expect(d.warnings.empty());
        }

        // P7. The rest of the runtime is ONE set: every non-declared runtime
        // file comes from one kind and one directory.
        std::set<std::string> origins;
        for (auto const* p : crt)
            if (p->kind != rp::Kind::Declared)
                origins.insert(std::format("{}:{}", rp::to_string(p->kind),
                                           p->sources.front().parent_path().string()));
        expect(le(origins.size(), 1u)) << "the runtime set was mixed";

        // P8. Which set: the toolset's, unless the dependency's is complete,
        // readable and strictly newer (D1); without the toolset's, the
        // dependency's is the only one.
        const bool derivedWins = derived.has_value()
            && (!toolset || (complete && *derived == Ver::New));
        const bool anyPlaced = !origins.empty();
        if (policy == rp::CrtPolicy::Carry)
            expect(anyPlaced || (declared && !toolset));
        if (anyPlaced) {
            const bool fromDerived = origins.begin()->starts_with("derived:");
            expect(eq(fromDerived, derivedWins));
        }
        // P9. Under self-contained nothing is placed unless something brings
        // a runtime name.
        if (policy == rp::CrtPolicy::Static && !declared && !derived)
            expect(crt.empty());

        // P10. A dependency's set that is not placed is stated once, as a
        // packaging fault; one that is placed over the toolset's is stated
        // as newer.
        if (derived && toolset) {
            const auto fault = std::ranges::count_if(d.notes, [](auto const& n) {
                return n.find("does not carry the compiler's runtime") != std::string::npos; });
            expect(eq(fault, derivedWins ? 0 : 1));
            if (derivedWins)
                expect(std::ranges::any_of(d.notes, [](auto const& n) {
                    return n.find("newer than the toolset's") != std::string::npos; }));
        }
        // P11. An unreadable version is said, and decides nothing.
        if (derived == Ver::Unreadable && toolset && complete)
            expect(std::ranges::any_of(d.notes, [](auto const& n) {
                return n.find("could not be read") != std::string::npos; }));
    }
    expect(gt(cases, 100));
}};

// Two search directories offering one ordinary name stay two sources of one
// destination (SPEC-007 R4.2): `mcpp stage` compares their bytes.
UTest two_derived_copies_of_an_ordinary_name_are_one_destination{"RuntimePlacement/TwoDerivedCopiesOfAnOrdinaryNameAreOneDestination", [] {
    rp::Input in;
    in.crt = rp::CrtPolicy::Carry;
    in.candidates.push_back({{"/a/libfoo.dll"}, "bin/libfoo.dll", rp::Kind::Derived});
    in.candidates.push_back({{"/b/libfoo.dll"}, "bin/libfoo.dll", rp::Kind::Derived});
    in.candidates.push_back({{"/c/libbar.dll"}, "bin/libbar.dll", rp::Kind::Derived});
    in.candidates.push_back({{"/p/libbar.dll"}, "bin/libbar.dll", rp::Kind::Declared});
    const auto d = rp::resolve(in);
    expect(fatal(eq(d.placed.size(), 2u)));
    auto foo = std::ranges::find_if(d.placed, [](auto const& p) { return p.dest == "bin/libfoo.dll"; });
    expect(fatal(ne(foo, d.placed.end())));
    expect(eq(foo->sources.size(), 2u));
    auto bar = std::ranges::find_if(d.placed, [](auto const& p) { return p.dest == "bin/libbar.dll"; });
    expect(fatal(ne(bar, d.placed.end())));
    expect(eq(bar->kind, rp::Kind::Declared));
    expect(eq(bar->sources, std::vector<fs::path>{"/p/libbar.dll"}));
}};

// A runtime name a declaration places under a subdirectory is not the
// program's runtime and is not governed by the rule.
UTest a_runtime_name_in_a_subdirectory_is_an_ordinary_file{"RuntimePlacement/ARuntimeNameInASubdirectoryIsAnOrdinaryFile", [] {
    rp::Input in;
    in.crt = rp::CrtPolicy::System;
    in.candidates.push_back({{"/p/vcruntime140.dll"}, "bin/plugins/vcruntime140.dll", rp::Kind::Declared});
    const auto d = rp::resolve(in);
    expect(d.errors.empty());
    expect(fatal(eq(d.placed.size(), 1u)));
    expect(eq(d.placed.front().dest, "bin/plugins/vcruntime140.dll"));
}};
