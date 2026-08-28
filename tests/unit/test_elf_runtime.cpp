#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.platform.elf_runtime;
import mcpp.platform.runtime_binding;
import mcpp.toolchain.post_install;
import mcpp.build.symbol_provision;

namespace elf = mcpp::platform::elf;
namespace runtime = mcpp::platform::runtime;
namespace tc = mcpp::toolchain;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_elf_runtime_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void put16(std::vector<unsigned char>& b, std::size_t p, std::uint16_t v) {
    b.at(p) = static_cast<unsigned char>(v);
    b.at(p + 1) = static_cast<unsigned char>(v >> 8);
}

void put32(std::vector<unsigned char>& b, std::size_t p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.at(p + static_cast<std::size_t>(i)) = static_cast<unsigned char>(v >> (i * 8));
}

void put64(std::vector<unsigned char>& b, std::size_t p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.at(p + static_cast<std::size_t>(i)) = static_cast<unsigned char>(v >> (i * 8));
}

std::uint32_t append_string(std::vector<unsigned char>& b,
                            std::size_t base,
                            std::size_t& cursor,
                            std::string_view value) {
    auto offset = static_cast<std::uint32_t>(cursor - base);
    for (char c : value) b.at(cursor++) = static_cast<unsigned char>(c);
    b.at(cursor++) = 0;
    return offset;
}

struct ElfFixtureSpec {
    std::string interp = "/store/glibc/2.44/lib64/ld-linux-x86-64.so.2";
    std::vector<std::string> needed = {"libc.so.6"};
    std::string runpath = "/host/z:/host/a";
};

// One deliberately tiny ELF64-LE image. It has no executable code; the test
// exercises the same program/dynamic/version tables real stripped binaries
// retain, without depending on readelf, the host compiler or the host libc.
std::filesystem::path write_elf_fixture(
    const std::filesystem::path& path,
    const ElfFixtureSpec& spec = {}) {
    constexpr std::uint64_t kVaddr = 0x400000;
    constexpr std::size_t kInterp = 0x200;
    constexpr std::size_t kDynamic = 0x300;
    constexpr std::size_t kDynstr = 0x500;
    constexpr std::size_t kVerneed = 0x600;
    constexpr std::size_t kVerdef = 0x680;
    std::vector<unsigned char> b(0x800, 0);

    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;  // ELFCLASS64
    b[5] = 1;  // ELFDATA2LSB
    b[6] = 1;  // EV_CURRENT
    put16(b, 0x10, 3);       // ET_DYN
    put16(b, 0x12, 62);      // EM_X86_64
    put32(b, 0x14, 1);
    put64(b, 0x20, 0x40);    // e_phoff
    put16(b, 0x34, 0x40);    // e_ehsize
    put16(b, 0x36, 0x38);    // e_phentsize
    put16(b, 0x38, 3);       // e_phnum

    auto ph = [&](std::size_t index, std::uint32_t type, std::uint64_t off,
                  std::uint64_t filesz) {
        auto p = 0x40 + index * 0x38;
        put32(b, p, type);
        put64(b, p + 0x08, off);
        put64(b, p + 0x10, kVaddr + off);
        put64(b, p + 0x20, filesz);
        put64(b, p + 0x28, filesz);
    };

    const std::string_view interp = spec.interp;
    std::copy(interp.begin(), interp.end(), b.begin() + kInterp);
    b[kInterp + interp.size()] = 0;
    ph(0, 1, 0, b.size());                       // PT_LOAD
    ph(1, 3, kInterp, interp.size() + 1);        // PT_INTERP
    ph(2, 2, kDynamic, 24 * 16);                 // PT_DYNAMIC

    std::size_t cursor = kDynstr;
    b[cursor++] = 0;
    std::vector<std::uint32_t> needed;
    for (auto const& name : spec.needed)
        needed.push_back(append_string(b, kDynstr, cursor, name));
    auto versionOwner = needed.empty()
        ? append_string(b, kDynstr, cursor, "libc.so.6")
        : needed.front();
    auto rpath = append_string(b, kDynstr, cursor, "/legacy/ignored");
    auto runpath = append_string(b, kDynstr, cursor, spec.runpath);
    auto needVersion = append_string(b, kDynstr, cursor, "GLIBC_2.40");
    auto defVersion = append_string(b, kDynstr, cursor, "GLIBC_2.44");
    auto dynstrSize = cursor - kDynstr;

    std::size_t d = kDynamic;
    auto dyn = [&](std::int64_t tag, std::uint64_t value) {
        put64(b, d, static_cast<std::uint64_t>(tag));
        put64(b, d + 8, value);
        d += 16;
    };
    dyn(5, kVaddr + kDynstr);            // DT_STRTAB
    dyn(10, dynstrSize);                 // DT_STRSZ
    for (auto offset : needed) dyn(1, offset); // DT_NEEDED
    dyn(15, rpath);                      // DT_RPATH (ignored when RUNPATH exists)
    dyn(29, runpath);                    // DT_RUNPATH
    dyn(0x6ffffffe, kVaddr + kVerneed);  // DT_VERNEED
    dyn(0x6fffffff, 1);                  // DT_VERNEEDNUM
    dyn(0x6ffffffc, kVaddr + kVerdef);   // DT_VERDEF
    dyn(0x6ffffffd, 1);                  // DT_VERDEFNUM
    dyn(0, 0);                           // DT_NULL

    // Elf64_Verneed + one Elf64_Vernaux.
    put16(b, kVerneed, 1);
    put16(b, kVerneed + 2, 1);
    put32(b, kVerneed + 4, versionOwner);
    put32(b, kVerneed + 8, 16);
    put32(b, kVerneed + 12, 0);
    put32(b, kVerneed + 16, 0);
    put16(b, kVerneed + 20, 0);
    put16(b, kVerneed + 22, 2);
    put32(b, kVerneed + 24, needVersion);
    put32(b, kVerneed + 28, 0);

    // Elf64_Verdef + one Elf64_Verdaux.
    put16(b, kVerdef, 1);
    put16(b, kVerdef + 2, 0);
    put16(b, kVerdef + 4, 2);
    put16(b, kVerdef + 6, 1);
    put32(b, kVerdef + 8, 0);
    put32(b, kVerdef + 12, 20);
    put32(b, kVerdef + 16, 0);
    put32(b, kVerdef + 20, defVersion);
    put32(b, kVerdef + 24, 0);

    std::ofstream os(path, std::ios::binary);
    os.write(reinterpret_cast<const char*>(b.data()),
             static_cast<std::streamsize>(b.size()));
    return path;
}

runtime::RuntimeBinding binding_for(const std::filesystem::path& payload,
                                    std::string version = "2.44") {
    runtime::RuntimeBinding b;
    b.schema = 1;
    b.providerId = "xlings";
    b.platform = "linux";
    b.arch = "x86_64";
    // A SubOS that described itself — which is what every fixture here means.
    // Left at the default the two states would be indistinguishable in the
    // fixtures, and "undeclared" carries its own verdict now.
    b.declared = true;
    b.runtimeId = "glibc@" + version;
    b.libc = b.runtimeId;
    b.loader = payload / version / "lib64" / "ld-linux-x86-64.so.2";
    b.libraryDirs = {payload / version / "lib64"};
    return b;
}

elf::ElfRuntimeFacts facts(std::filesystem::path path,
                           std::vector<std::string> required = {},
                           std::vector<std::string> defined = {}) {
    elf::ElfRuntimeFacts out;
    out.artifact = std::move(path);
    out.requiredGlibcVersions = std::move(required);
    out.definedGlibcVersions = std::move(defined);
    return out;
}

TEST(RuntimePayload, ExactBindingWinsRegardlessOfDirectoryOrder) {
    Tmp t;
    auto root = t.path / "xim-x-glibc";
    std::filesystem::create_directories(root / "2.39" / "lib64");
    std::filesystem::create_directories(root / "2.44" / "lib64");
    std::ofstream(root / "2.39" / "lib64" / "ld-linux-x86-64.so.2") << "x";
    std::ofstream(root / "2.44" / "lib64" / "ld-linux-x86-64.so.2") << "x";

    auto selected = tc::select_glibc_payload_lib(root, "glibc@2.44");
    ASSERT_TRUE(selected.has_value()) << selected.error();
    EXPECT_EQ(*selected, root / "2.44" / "lib64");
}

TEST(RuntimePayload, MissingOrMalformedBindingIsAnError) {
    Tmp t;
    auto root = t.path / "xim-x-glibc";
    std::filesystem::create_directories(root / "2.39" / "lib64");
    std::ofstream(root / "2.39" / "lib64" / "ld-linux-x86-64.so.2") << "x";

    for (auto id : {"", "glibc", "glibc@2.44", "musl@1.2.5", "glibc@../2.39"}) {
        auto selected = tc::select_glibc_payload_lib(root, id);
        EXPECT_FALSE(selected.has_value()) << id;
        if (!selected) EXPECT_FALSE(selected.error().empty());
    }
}

TEST(ElfRuntime, ParsesProgramDynamicAndGnuVersionTablesInternally) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF table inspection is exercised on native Linux";
    Tmp t;
    auto parsed = elf::inspect_elf_runtime(write_elf_fixture(t.path / "fixture"));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->interp,
              "/store/glibc/2.44/lib64/ld-linux-x86-64.so.2");
    EXPECT_EQ(parsed->runpaths,
              (std::vector<std::string>{"/host/z", "/host/a"}));
    EXPECT_EQ(parsed->needed, (std::vector<std::string>{"libc.so.6"}));
    EXPECT_EQ(parsed->requiredGlibcVersions,
              (std::vector<std::string>{"GLIBC_2.40"}));
    EXPECT_EQ(parsed->definedGlibcVersions,
              (std::vector<std::string>{"GLIBC_2.44"}));
}

TEST(ElfRuntime, RejectsUnsupportedOrTruncatedElfWithoutGuessing) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF table inspection is exercised on native Linux";
    Tmp t;
    std::ofstream(t.path / "text") << "not an ELF";
    auto text = elf::inspect_elf_runtime(t.path / "text");
    EXPECT_FALSE(text.has_value());

    std::ofstream tiny(t.path / "tiny", std::ios::binary);
    tiny.write("\177ELF\2\1", 6);
    tiny.close();
    auto truncated = elf::inspect_elf_runtime(t.path / "tiny");
    EXPECT_FALSE(truncated.has_value());
}

TEST(ElfRuntime, ReusesAnAlreadyLoadedSonameAcrossDependencyRunpaths) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto payload = t.path / "store";
    auto glibc39 = payload / "2.39" / "lib64";
    auto glibc44 = payload / "2.44" / "lib64";
    auto shimDir = t.path / "shim";
    std::filesystem::create_directories(glibc39);
    std::filesystem::create_directories(glibc44);
    std::filesystem::create_directories(shimDir);

    write_elf_fixture(glibc39 / "libc.so.6", {
        .needed = {"libc.so.6"},
        .runpath = glibc39.string(),
    });
    write_elf_fixture(glibc44 / "libc.so.6", {
        .needed = {"libc.so.6"},
        .runpath = glibc44.string(),
    });
    write_elf_fixture(shimDir / "libshim.so", {
        .needed = {"libc.so.6"},
        .runpath = glibc39.string(),
    });
    auto app = write_elf_fixture(t.path / "app", {
        .needed = {"libc.so.6", "libshim.so"},
        .runpath = shimDir.string(),
    });

    auto resolution = elf::resolve_runtime_closure(app, binding_for(payload));
    ASSERT_TRUE(resolution.unresolved.empty());
    ASSERT_EQ(resolution.resolvedLibcs.size(), 1u)
        << "a later dependency must reuse the process-global libc SONAME";
    EXPECT_EQ(resolution.resolvedLibcs.front(),
              std::filesystem::weakly_canonical(glibc44 / "libc.so.6"));
}

TEST(RuntimePhysics, RuleBRejectsInterpreterAndLibcFromDifferentPayloads) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = t.path / "store" / "2.39" / "lib64" / "libc.so.6";
    r.objects.push_back(facts(r.artifact.resolvedLibc, {}, {"GLIBC_2.39"}));

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::ProvenMismatch);
    EXPECT_NE(verdict.explain().find("rule B"), std::string::npos);
}

TEST(RuntimePhysics, RuleBAcceptsSameSelectedPayload) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app", {"GLIBC_2.39"});
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.objects.push_back(facts(r.artifact.resolvedLibc, {}, {"GLIBC_2.39", "GLIBC_2.44"}));

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Pass);
}

TEST(RuntimePhysics, RuleBRejectsTwoLibcsAcrossTheResolvedClosure) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.resolvedLibcs = {
        r.artifact.resolvedLibc,
        t.path / "store" / "2.39" / "lib64" / "libc.so.6"};
    r.objects.push_back(facts(r.artifact.resolvedLibc, {}, {"GLIBC_2.44"}));

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::ProvenMismatch);
    EXPECT_NE(verdict.explain().find("more than one libc payload"),
              std::string::npos);
}

TEST(RuntimePhysics, RuleARejectsRequiredFloorAboveSelectedLibcExports) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store", "2.39");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.objects.push_back(facts(r.artifact.resolvedLibc, {}, {"GLIBC_2.39"}));
    r.objects.push_back(facts("/lib64/libtinfo.so.6", {"GLIBC_2.42"}));

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::ProvenMismatch);
    EXPECT_NE(verdict.explain().find("rule A"), std::string::npos);
    EXPECT_NE(verdict.explain().find("libtinfo.so.6"), std::string::npos);
}

TEST(RuntimePhysics, RuleAAcceptsEqualOrLowerFloor) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.objects.push_back(facts(r.artifact.resolvedLibc, {}, {"GLIBC_2.39", "GLIBC_2.44"}));
    r.objects.push_back(facts("/host/lib.so", {"GLIBC_2.39", "GLIBC_2.44"}));

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Pass);
}

// UNDER A HERMETIC BINDING, "not found" IS A MEASUREMENT.
//
// The artifact's PT_INTERP names a private loader whose entire search path mcpp
// computed — RPATH/RUNPATH, payloads, SubOS farm, and nothing else: no host
// defaults and no ld.so.cache. So an unresolved DT_NEEDED is the same answer
// the loader will give, and the program cannot start. Filing that under
// `inconclusive` reports a proven failure as an absence of one, and it is how a
// GL program that exited 127 was shipped as `validation: pass`.
TEST(RuntimePhysics, UnresolvedNeededUnderHermeticBindingIsProven) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    ASSERT_TRUE(b.hermetic()) << "this test's premise is a private loader";
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.unresolved = {"libgpu-driver.so"};
    r.unresolvedSonames = {"libgpu-driver.so"};   // a NEEDED nothing provides

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Unresolvable);
    EXPECT_TRUE(verdict.blocking())
        << "a proven-unstartable artifact must fail the build, not warn";
    EXPECT_NE(verdict.explain().find("libgpu-driver.so"), std::string::npos);
}

// `allow_host_libs` opts out of BOTH phases, or it means two different things.
//
// It already switches off the link-time hermeticity check. Once the user has
// declared that this build reaches outside the sandbox, mcpp cannot also claim
// the artifact is unstartable — they may run it under LD_LIBRARY_PATH, or where
// the library is installed somewhere the private loader does look. It reports;
// it does not block. (Without this, e2e 206's deliberate host-DSO control
// failed its own build.)
TEST(RuntimePhysics, AllowHostLibsDowngradesTheProofToAReport) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.unresolved = {"libtinfo.so.6"};
    r.unresolvedSonames = {"libtinfo.so.6"};

    auto blocked = elf::validate_runtime_artifact(r.artifact.artifact, b, r,
                                                  /*hostLibsAllowed=*/false);
    EXPECT_EQ(blocked.status, elf::RuntimeVerdict::Status::Unresolvable);
    EXPECT_TRUE(blocked.blocking());

    auto reported = elf::validate_runtime_artifact(r.artifact.artifact, b, r,
                                                   /*hostLibsAllowed=*/true);
    EXPECT_EQ(reported.status, elf::RuntimeVerdict::Status::Inconclusive);
    EXPECT_FALSE(reported.blocking());
    EXPECT_NE(reported.explain().find("libtinfo.so.6"), std::string::npos);
    EXPECT_NE(reported.explain().find("allow_host_libs"), std::string::npos)
        << "a downgraded verdict must say what downgraded it";
}

// THE ARTIFACT'S FORMAT DECIDES, NOT THE BINDING'S.
//
// A Linux→Windows cross build runs with the HOST's binding — Linux, glibc, a
// private loader, therefore hermetic — while producing a PE. The parse failure
// ("not an ELF file") landed in `unresolved`, and the artifact was failed for a
// missing shared library it could not possibly have. CI caught it; this pins it.
TEST(RuntimePhysics, CrossBuiltNonElfArtifactIsNotJudgedByElfRules) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    ASSERT_TRUE(b.hermetic());
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "crosswin.exe");
    r.artifactIsElf = false;                       // what the PE walk produces
    r.unresolved = {"artifact 'crosswin.exe' is not ELF"};
    // and crucially NO unresolvedSonames — nothing was ever looked for.

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Pass);
    EXPECT_FALSE(verdict.blocking());
    EXPECT_NE(verdict.explain().find("not ELF"), std::string::npos);
}

// The narrower half of the same conflation. Even on a genuine ELF, "I could not
// read one of the objects" or "I stopped after 512" are statements about the
// CHECK — a check that could not look has proven nothing, so it stays
// inconclusive no matter how hermetic the binding is.
TEST(RuntimePhysics, UnreadableObjectIsInconclusiveEvenWhenHermetic) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = b.loader->string();
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.unresolved = {"libweird.so.1 (truncated ELF header)"};   // read, not missing
    // no unresolvedSonames: it WAS found, it just could not be parsed

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Inconclusive);
    EXPECT_FALSE(verdict.blocking());
}

// The other side of the same line. Without a private loader the artifact runs
// under the HOST's, which also consults `ld.so.cache` — something mcpp
// deliberately does not parse. There, unresolved really is unknown, and
// claiming otherwise would fail builds that work.
TEST(RuntimePhysics, UnresolvedNeededWithoutAPrivateLoaderStaysInconclusive) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    b.loader.reset();                       // host runtime: gcc@system and friends
    ASSERT_FALSE(b.hermetic());
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.resolvedLibc = b.libraryDirs.front() / "libc.so.6";
    r.unresolved = {"libgpu-driver.so"};

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Inconclusive);
    EXPECT_FALSE(verdict.blocking());
    EXPECT_NE(verdict.explain().find("libgpu-driver.so"), std::string::npos);
}

// An undeclared SubOS on Linux is not "some other runtime" — it is a runtime
// nobody described, and the two must not print the same sentence. Reporting the
// second as the first sends the reader looking for a runtime they did not
// select, and quietly counts "unknown" as "fine".
TEST(RuntimePhysics, UndeclaredBindingIsInconclusiveNotNotApplicable) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF/glibc runtime physics only apply on Linux";
    Tmp t;
    auto b = binding_for(t.path / "store");
    b.declared = false;
    b.runtimeId.clear();
    b.loader.reset();
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Inconclusive);
    EXPECT_NE(verdict.explain().find("does not describe its runtime"),
              std::string::npos);
}

TEST(RuntimePhysics, NonLinuxValidatorIsATypedNoop) {
    if constexpr (mcpp::platform::is_linux)
        GTEST_SKIP() << "the non-Linux boundary is exercised on native runners";
    Tmp t;
    auto b = binding_for(t.path / "store");
    elf::RuntimeResolution r;
    r.artifactIsElf = true;
    r.artifact = facts(t.path / "app");
    r.artifact.interp = "/deliberately/mismatched/loader";
    r.unresolved = {"libgpu-driver.so"};

    auto verdict = elf::validate_runtime_artifact(r.artifact.artifact, b, r);
    EXPECT_EQ(verdict.status, elf::RuntimeVerdict::Status::Pass);
    EXPECT_NE(verdict.explain().find("non-Linux"), std::string::npos);
}

} // namespace

// ── the dynamic symbol table (issue #519) ─────────────────────────────────
//
// These read THIS TEST BINARY. A hand-built ELF fixture can assert that the
// parser walks the structures it is given; only a real link can assert that
// the RULE is right about what a linker actually produces — and the rule was
// wrong twice before it was measured.

TEST(ElfRuntime, ReadsThisBinarysOwnDynamicSymbols) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF dynamic symbol tables only exist on Linux here";

    auto self = std::filesystem::path("/proc/self/exe");
    if (!std::filesystem::exists(self)) GTEST_SKIP() << "no /proc/self/exe";

    auto symbols = elf::inspect_dynamic_symbols(self);
    ASSERT_TRUE(symbols.has_value()) << symbols.error();
    if (!symbols->present)
        GTEST_SKIP() << "this test binary is statically linked";

    // The DENOMINATOR has to be real. `exported = 0` out of an unread table
    // is the reading this whole area exists to make impossible.
    EXPECT_GT(symbols->total, 0u);
    EXPECT_TRUE(symbols->copyRelocationsKnown)
        << "x86_64/aarch64/riscv64 must all be in the COPY relocation table";
}

TEST(ElfRuntime, ThisBinaryExportsNothingOfItsOwn) {
    if constexpr (!mcpp::platform::is_linux)
        GTEST_SKIP() << "ELF dynamic symbol tables only exist on Linux here";

    auto self = std::filesystem::path("/proc/self/exe");
    if (!std::filesystem::exists(self)) GTEST_SKIP() << "no /proc/self/exe";

    auto facts = elf::inspect_elf_runtime(self);
    ASSERT_TRUE(facts.has_value()) << facts.error();
    // ⚠️ PT_INTERP, not the ELF type: a PIE executable is ET_DYN exactly like
    // a shared library, and whether this binary is PIE is the payload
    // compiler's default rather than mcpp's decision.
    if (facts->interp.empty())
        GTEST_SKIP() << "statically linked: no loader, so no flat namespace";

    auto symbols = elf::inspect_dynamic_symbols(self);
    ASSERT_TRUE(symbols.has_value()) << symbols.error();
    ASSERT_TRUE(symbols->present);

    auto exported = mcpp::build::symbol_provision::exported_definitions(*symbols);
    ASSERT_TRUE(exported.has_value());

    // Measured on every mcpp binary: the only defined entries are glibc's
    // copy relocations (environ, __environ, stdout, stderr,
    // __libc_single_threaded), and `environ` is a WEAK ALIAS AT THE SAME
    // ADDRESS as `__environ` with no relocation entry of its own. A
    // name-keyed filter reports it in every executable ever built.
    std::vector<std::string> names;
    for (auto const& e : *exported) names.push_back(e.name);
    EXPECT_TRUE(names.empty())
        << "an ordinary mcpp-linked test binary must export nothing of its "
           "own; got: " << [&] {
               std::string s;
               for (auto const& n : names) { s += n; s += ' '; }
               return s;
           }();
}
