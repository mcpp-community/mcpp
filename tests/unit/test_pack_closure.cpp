#include <gtest/gtest.h>

import std;
import mcpp.pack;

// #634 A3: the closure `mcpp pack` reads from the files for the PE, Android and
// Mach-O rows. The rules are asserted here, over hand-built objects in a
// temporary tree, because two of the three rows cannot be produced on the
// machine that runs most of the suite: a Mach-O needs a macOS toolchain and a
// device-loadable Android tree needs an emulator to prove anything end to end.
// What the rule decides -- member, platform or unresolved, and where a member
// is staged -- is a pure function of the names, the rpaths and which files
// exist, and that is what these tests hold fixed.
//
// The fixtures follow `test_pack_binfmt.cpp`'s: written as plainly as possible,
// no spans and no lambdas that mutate captured state.

namespace {

namespace fs = std::filesystem;

void put(std::string& b, std::size_t at, std::uint64_t v, std::size_t width) {
    if (b.size() < at + width) b.resize(at + width, '\0');
    for (std::size_t i = 0; i < width; ++i)
        b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

struct Tree {
    fs::path root;
    Tree() {
        root = fs::temp_directory_path()
             / std::format("mcpp_pack_closure_{}", std::random_device{}());
        fs::create_directories(root);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    fs::path write(const fs::path& rel, const std::string& bytes) const {
        const auto p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream os(p, std::ios::binary);
        os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

// A minimal ELF64 shared object whose PT_DYNAMIC carries `needed` as DT_NEEDED
// entries; the same layout `test_pack_binfmt.cpp` documents.
std::string elf_needing(const std::vector<std::string>& needed) {
    std::string b(64, '\0');
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2; b[5] = 1; b[6] = 1;
    put(b, 16, 3, 2);
    put(b, 18, 0x3E, 2);
    put(b, 20, 1, 4);
    put(b, 32, 64, 8);
    put(b, 52, 64, 2);
    put(b, 54, 56, 2);
    put(b, 56, 2, 2);

    const std::size_t dynAt = 64 + 2 * 56;
    const std::size_t dynSize = (needed.size() + 2) * 16;
    const std::size_t strAt = dynAt + dynSize;
    std::string strtab(1, '\0');
    std::vector<std::size_t> offsets;
    for (std::size_t k = 0; k < needed.size(); ++k) {
        offsets.push_back(strtab.size());
        strtab += needed[k];
        strtab.push_back('\0');
    }
    const std::size_t total = strAt + strtab.size();

    put(b, 64 + 0, 1, 4);
    put(b, 64 + 4, 5, 4);
    put(b, 64 + 8, 0, 8);
    put(b, 64 + 16, 0, 8);
    put(b, 64 + 32, total, 8);
    put(b, 64 + 40, total, 8);
    put(b, 120 + 0, 2, 4);
    put(b, 120 + 8, dynAt, 8);
    put(b, 120 + 16, dynAt, 8);
    put(b, 120 + 32, dynSize, 8);

    std::size_t at = dynAt;
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        put(b, at, 1, 8);
        put(b, at + 8, offsets[k], 8);
        at += 16;
    }
    put(b, at, 5, 8);
    put(b, at + 8, strAt, 8);
    at += 16;
    put(b, at, 0, 8);
    put(b, at + 8, 0, 8);

    b.resize(total, '\0');
    for (std::size_t i = 0; i < strtab.size(); ++i) b[strAt + i] = strtab[i];
    return b;
}

// A minimal PE32+ image importing `imports` through its import directory.
std::string pe_importing(const std::vector<std::string>& imports) {
    const std::size_t   kNt = 0x40;
    const std::size_t   kOptSize = 0xF0;
    const std::size_t   kSecAt = kNt + 24 + kOptSize;
    const std::size_t   kRawAt = 0x400;
    const std::uint32_t kSecVa = 0x1000;
    const std::size_t   dirsAt = kNt + 24 + 112;

    std::string b(kRawAt, '\0');
    b[0] = 'M'; b[1] = 'Z';
    put(b, 0x3C, kNt, 4);
    b[kNt] = 'P'; b[kNt + 1] = 'E';
    put(b, kNt + 4, 0x8664, 2);
    put(b, kNt + 6, 1, 2);
    put(b, kNt + 20, kOptSize, 2);
    put(b, kNt + 24, 0x20b, 2);
    put(b, dirsAt - 4, 16, 4);
    const std::string secName = ".rdata";
    for (std::size_t i = 0; i < secName.size(); ++i) b[kSecAt + i] = secName[i];
    put(b, kSecAt + 8, 0x1000, 4);
    put(b, kSecAt + 12, kSecVa, 4);
    put(b, kSecAt + 16, 0x1000, 4);
    put(b, kSecAt + 20, kRawAt, 4);

    // `std::size_t` elements, not `std::uint32_t`: on macos-15 (clang 22,
    // libc++ as a module) the first `push_back` into a `std::vector<std::uint32_t>`
    // here faulted in `memmove` at address 0 (lldb, run 34819771218), while
    // `elf_needing`'s `std::vector<std::size_t>` runs. The values fit either.
    std::vector<std::size_t> nameRvas;
    std::size_t cursor = kRawAt;
    for (std::size_t k = 0; k < imports.size(); ++k) {
        nameRvas.push_back(kSecVa + (cursor - kRawAt));
        for (std::size_t i = 0; i < imports[k].size(); ++i) {
            put(b, cursor, static_cast<unsigned char>(imports[k][i]), 1);
            ++cursor;
        }
        put(b, cursor, 0, 1);
        ++cursor;
    }
    cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
    const std::size_t importAt = cursor;
    for (std::size_t k = 0; k < nameRvas.size(); ++k) {
        put(b, cursor + 0, 0x9000, 4);
        put(b, cursor + 12, nameRvas[k], 4);
        put(b, cursor + 16, 0x9100, 4);
        cursor += 20;
    }
    for (std::size_t i = 0; i < 20; ++i) { put(b, cursor, 0, 1); ++cursor; }
    b.resize(std::max(b.size(), kRawAt + 0x1000), '\0');
    if (!imports.empty()) {
        put(b, dirsAt + 1 * 8, static_cast<std::uint32_t>(kSecVa + (importAt - kRawAt)), 4);
        put(b, dirsAt + 1 * 8 + 4, 20 * (imports.size() + 1), 4);
    }
    return b;
}

// A thin little-endian 64-bit arm64 Mach-O naming `dylibs` with LC_LOAD_DYLIB
// and carrying `rpaths` as LC_RPATH, in that order.
std::string macho_naming(const std::vector<std::string>& dylibs,
                         const std::vector<std::string>& rpaths) {
    std::string b(32, '\0');
    b[0] = '\xcf'; b[1] = '\xfa'; b[2] = '\xed'; b[3] = '\xfe';
    put(b, 4, 0x0100000c, 4);
    put(b, 12, 2, 4);
    put(b, 16, dylibs.size() + rpaths.size(), 4);
    std::size_t cursor = 32;
    for (std::size_t k = 0; k < dylibs.size() + rpaths.size(); ++k) {
        const bool isDylib = k < dylibs.size();
        const std::string& str = isDylib ? dylibs[k] : rpaths[k - dylibs.size()];
        const std::size_t structSize = isDylib ? 24 : 12;
        const std::size_t cmdsize = (structSize + str.size() + 1 + 3) & ~std::size_t(3);
        b.resize(cursor + cmdsize, '\0');
        put(b, cursor + 0, isDylib ? 0x0000000c : 0x8000001c, 4);
        put(b, cursor + 4, cmdsize, 4);
        put(b, cursor + 8, structSize, 4);
        for (std::size_t i = 0; i < str.size(); ++i) b[cursor + structSize + i] = str[i];
        cursor += cmdsize;
    }
    put(b, 20, cursor - 32, 4);
    return b;
}

std::vector<std::string> member_dests(const mcpp::pack::ClosureRead& r) {
    std::vector<std::string> out;
    for (auto const& m : r.members) out.push_back(m.dest.generic_string());
    return out;
}

std::vector<std::string> unresolved_names(const mcpp::pack::ClosureRead& r) {
    std::vector<std::string> out;
    for (auto const& u : r.unresolved) out.push_back(u.name);
    return out;
}

using Names = std::vector<std::string>;

} // namespace

// ── Android ──────────────────────────────────────────────────────────────

TEST(PackClosureAndroid, TheStubDirectoryIsTheDevicesAndEveryOtherNameTravels) {
    Tree t;
    t.write("platform/libc.so", "stub");
    t.write("platform/libm.so", "stub");
    auto app = t.write("build/libapp.so",
                       elf_needing({"libfw.so", "libc++_shared.so", "libc.so"}));
    t.write("build/libfw.so", elf_needing({"libc++_shared.so", "libm.so"}));
    t.write("ndk/libc++_shared.so", elf_needing({"libc.so"}));

    mcpp::pack::ClosureReadInput in;
    in.object       = app;
    in.rule         = mcpp::pack::ClosureRule::Android;
    // The driver's search list holds the stub directory too; the platform
    // check runs first, so a stub is never staged.
    in.searchDirs   = {t.root / "build", t.root / "platform", t.root / "ndk"};
    in.platformDirs = {t.root / "platform"};
    auto r = mcpp::pack::read_closure(in);

    EXPECT_EQ(member_dests(r), (Names{"libc++_shared.so", "libfw.so"}));
    EXPECT_EQ(r.platform, (Names{"libc.so", "libm.so"}));
    EXPECT_TRUE(r.unresolved.empty());
    ASSERT_EQ(r.members.size(), 2u);
    EXPECT_EQ(r.members[0].source, t.root / "ndk" / "libc++_shared.so");
}

TEST(PackClosureAndroid, ANameFoundNowhereLeavesTheClosureIncomplete) {
    Tree t;
    t.write("platform/libc.so", "stub");
    auto app = t.write("build/libapp.so", elf_needing({"libgone.so", "libc.so"}));

    mcpp::pack::ClosureReadInput in;
    in.object       = app;
    in.rule         = mcpp::pack::ClosureRule::Android;
    in.searchDirs   = {t.root / "build"};
    in.platformDirs = {t.root / "platform"};
    auto r = mcpp::pack::read_closure(in);

    EXPECT_TRUE(r.members.empty());
    EXPECT_EQ(unresolved_names(r), (Names{"libgone.so"}));
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_NE(r.unresolved[0].why.find((t.root / "build").string()), std::string::npos)
        << "the reason names where it looked: " << r.unresolved[0].why;
}

// ── PE, unchanged by the generalisation ──────────────────────────────────

TEST(PackClosurePe, SystemNamesAndNamesFoundNowhereAreTheTargets) {
    Tree t;
    auto exe = t.write("bin/app.exe",
                       pe_importing({"KERNEL32.dll", "foo.dll", "missing.dll"}));
    t.write("deps/foo.dll", pe_importing({"bar.dll"}));
    t.write("deps/bar.dll", pe_importing({}));

    mcpp::pack::ClosureReadInput in;
    in.object     = exe;
    in.rule       = mcpp::pack::ClosureRule::Pe;
    in.searchDirs = {t.root / "bin", t.root / "deps"};
    auto r = mcpp::pack::read_closure(in);

    EXPECT_EQ(member_dests(r), (Names{"bar.dll", "foo.dll"}));
    EXPECT_EQ(r.platform, (Names{"KERNEL32.dll", "missing.dll"}));
    EXPECT_TRUE(r.unresolved.empty()) << "PE's rule resolves every name";
}

TEST(PackClosurePe, ForceBundleReachesTheSystemList) {
    Tree t;
    auto exe = t.write("bin/app.exe", pe_importing({"dbghelp.dll"}));
    t.write("deps/dbghelp.dll", pe_importing({}));

    mcpp::pack::ClosureReadInput in;
    in.object      = exe;
    in.rule        = mcpp::pack::ClosureRule::Pe;
    in.searchDirs  = {t.root / "deps"};
    auto without = mcpp::pack::read_closure(in);
    EXPECT_EQ(without.platform, (Names{"dbghelp.dll"}));

    in.forceBundle = {"dbghelp.dll"};
    auto with = mcpp::pack::read_closure(in);
    EXPECT_EQ(member_dests(with), (Names{"dbghelp.dll"}));
    EXPECT_TRUE(with.platform.empty());
}

// ── Mach-O ───────────────────────────────────────────────────────────────

TEST(PackClosureMachO, AnRpathDylibBesideTheProgramIsAMemberTransitively) {
    Tree t;
    auto app = t.write("bin/app",
        macho_naming({"@rpath/libfw.dylib", "/usr/lib/libSystem.B.dylib"},
                     {"@loader_path"}));
    // libfw carries no rpath of its own: it inherits the program's.
    t.write("bin/libfw.dylib",
        macho_naming({"@rpath/libdep.dylib", "/usr/lib/libc++.1.dylib"}, {}));
    t.write("bin/libdep.dylib", macho_naming({"/usr/lib/libSystem.B.dylib"}, {}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    in.arch   = "aarch64";
    auto r = mcpp::pack::read_closure(in);

    EXPECT_EQ(member_dests(r), (Names{"libdep.dylib", "libfw.dylib"}));
    EXPECT_EQ(r.platform, (Names{"/usr/lib/libSystem.B.dylib", "/usr/lib/libc++.1.dylib"}));
    EXPECT_TRUE(r.unresolved.empty());
}

TEST(PackClosureMachO, ATrailingSlashAndExecutablePathQualifyAsTheProgramsDirectory) {
    Tree t;
    auto app = t.write("bin/app",
        macho_naming({"@rpath/liba.dylib", "@executable_path/libb.dylib"},
                     {"@executable_path/"}));
    t.write("bin/liba.dylib", macho_naming({}, {}));
    t.write("bin/libb.dylib", macho_naming({}, {}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_EQ(member_dests(r), (Names{"liba.dylib", "libb.dylib"}));
    EXPECT_TRUE(r.unresolved.empty());
}

TEST(PackClosureMachO, AnAbsoluteInstallNameOutsideTheOsRootsIsUnresolved) {
    Tree t;
    // The file exists on this machine; the loader on another one reads that
    // path, not the tree, so a copy would not be what loads. An install name
    // is a POSIX path: on a Windows host the temporary directory is not one,
    // so the name is spelled as a macOS path there, and the rule is the same
    // whether or not the file exists.
    auto elsewhere = t.write("opt/libq.dylib", macho_naming({}, {}));
    const std::string installName = elsewhere.string().starts_with('/')
        ? elsewhere.string() : std::string("/opt/elsewhere/libq.dylib");
    auto app = t.write("bin/app", macho_naming({installName}, {"@loader_path"}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_TRUE(r.members.empty());
    EXPECT_EQ(unresolved_names(r), (Names{installName}));
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_NE(r.unresolved[0].why.find("absolute install name"), std::string::npos)
        << r.unresolved[0].why;
}

TEST(PackClosureMachO, AnRpathOnlyAnAbsoluteEntryReachesIsUnresolved) {
    Tree t;
    t.write("store/libfw.dylib", macho_naming({}, {}));
    auto app = t.write("bin/app",
        macho_naming({"@rpath/libfw.dylib"}, {(t.root / "store").string()}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_TRUE(r.members.empty());
    EXPECT_EQ(unresolved_names(r), (Names{"@rpath/libfw.dylib"}));
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_NE(r.unresolved[0].why.find("@loader_path"), std::string::npos)
        << r.unresolved[0].why;

    // The negative direction: the same dylib staged through a qualifying
    // rpath is a member, sourced from wherever the loader finds it here.
    auto app2 = t.write("bin2/app",
        macho_naming({"@rpath/libfw.dylib"},
                     {(t.root / "store").string(), "@loader_path"}));
    in.object = app2;
    auto r2 = mcpp::pack::read_closure(in);
    EXPECT_EQ(member_dests(r2), (Names{"libfw.dylib"}));
    ASSERT_EQ(r2.members.size(), 1u);
    EXPECT_EQ(r2.members[0].source, t.root / "store" / "libfw.dylib");
}

TEST(PackClosureMachO, ALoaderPathSubdirectoryIsStagedAtThatSubdirectory) {
    Tree t;
    auto app = t.write("bin/app", macho_naming({"@loader_path/plugins/libp.dylib"}, {}));
    t.write("bin/plugins/libp.dylib", macho_naming({"@loader_path/libq.dylib"}, {}));
    t.write("bin/plugins/libq.dylib", macho_naming({}, {}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_EQ(member_dests(r), (Names{"plugins/libp.dylib", "plugins/libq.dylib"}));
    EXPECT_TRUE(r.unresolved.empty());
}

TEST(PackClosureMachO, ANameOutsideTheProgramsDirectoryIsUnresolved) {
    Tree t;
    t.write("Frameworks/libz.dylib", macho_naming({}, {}));
    auto app = t.write("bin/app",
        macho_naming({"@executable_path/../Frameworks/libz.dylib"}, {}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_TRUE(r.members.empty());
    EXPECT_EQ(unresolved_names(r), (Names{"@executable_path/../Frameworks/libz.dylib"}));
}

TEST(PackClosureMachO, AMissingDylibIsUnresolvedAndNamed) {
    Tree t;
    auto app = t.write("bin/app", macho_naming({"@rpath/libgone.dylib"}, {"@loader_path"}));

    mcpp::pack::ClosureReadInput in;
    in.object = app;
    in.rule   = mcpp::pack::ClosureRule::MachO;
    auto r = mcpp::pack::read_closure(in);
    EXPECT_TRUE(r.members.empty());
    EXPECT_EQ(unresolved_names(r), (Names{"@rpath/libgone.dylib"}));
}
