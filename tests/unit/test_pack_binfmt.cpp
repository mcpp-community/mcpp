#include <gtest/gtest.h>
#include <cstdio>       // stderr is a MACRO — `import std;` cannot export it
#include <cstdlib>      // getenv

import std;
import mcpp.pack.binfmt;
import mcpp.pack.zip;

namespace bf = mcpp::pack::binfmt;

// WHY THE FIXTURES ARE SYNTHESIZED RATHER THAN CHECKED IN.
//
// This module's whole reason to exist is that a dependency closure must be
// derivable WITHOUT running the binary — so that a Linux runner can package a
// Windows build. A test that needed a real PE would need a Windows toolchain
// to produce one, which is the same dependency one layer up: the test would
// only run where the feature was already unnecessary.
//
// Hand-built headers also make the assertions exact. "Found two DLLs" is a
// much weaker statement than "found the two names I wrote at these RVAs, and
// not the terminator".

namespace {

void put(std::string& b, std::size_t at, std::uint64_t v, std::size_t width) {
    if (b.size() < at + width) b.resize(at + width, '\0');
    for (std::size_t i = 0; i < width; ++i)
        b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

std::filesystem::path write_temp(std::string_view tag, std::string_view bytes) {
    auto p = std::filesystem::temp_directory_path()
           / std::format("mcpp-binfmt-{}-{}", tag,
                         std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream os(p, std::ios::binary);
    os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}

// ─── a minimal ELF64 shared object ──────────────────────────────────────
//
// One PT_LOAD covering the whole file at vaddr 0 (so vaddr == file offset,
// which keeps the fixture readable without making the parser's translation
// step a no-op — DT_STRTAB is still resolved THROUGH the segment table), plus
// a PT_DYNAMIC carrying two DT_NEEDED and a DT_STRTAB.
std::string elf_with_needed(std::span<const std::string_view> needed) {
    std::string b;
    b.resize(64, '\0');
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;            // ELFCLASS64
    b[5] = 1;            // ELFDATA2LSB
    b[6] = 1;            // EV_CURRENT
    put(b, 16, 3, 2);          // e_type = ET_DYN
    put(b, 18, 0x3E, 2);       // e_machine = EM_X86_64
    put(b, 20, 1, 4);          // e_version
    put(b, 32, 64, 8);         // e_phoff
    put(b, 52, 64, 2);         // e_ehsize
    put(b, 54, 56, 2);         // e_phentsize
    put(b, 56, 2, 2);          // e_phnum

    const std::size_t dynAt = 64 + 2 * 56;
    const std::size_t dynEntries = needed.size() + 2;    // + DT_STRTAB + DT_NULL
    const std::size_t dynSize = dynEntries * 16;
    const std::size_t strAt = dynAt + dynSize;

    std::string strtab;
    strtab.push_back('\0');                              // index 0 is empty
    std::vector<std::size_t> offsets;
    for (auto n : needed) {
        offsets.push_back(strtab.size());
        strtab.append(n);
        strtab.push_back('\0');
    }
    const std::size_t total = strAt + strtab.size();

    // PT_LOAD: the whole file, vaddr 0.
    put(b, 64 + 0,  1, 4);          // p_type
    put(b, 64 + 4,  5, 4);          // p_flags
    put(b, 64 + 8,  0, 8);          // p_offset
    put(b, 64 + 16, 0, 8);          // p_vaddr
    put(b, 64 + 32, total, 8);      // p_filesz
    put(b, 64 + 40, total, 8);      // p_memsz

    // PT_DYNAMIC
    put(b, 120 + 0,  2, 4);
    put(b, 120 + 8,  dynAt, 8);
    put(b, 120 + 16, dynAt, 8);
    put(b, 120 + 32, dynSize, 8);

    std::size_t at = dynAt;
    for (auto off : offsets) {
        put(b, at, 1, 8);                 // DT_NEEDED
        put(b, at + 8, off, 8);
        at += 16;
    }
    put(b, at, 5, 8);                     // DT_STRTAB (a VIRTUAL ADDRESS)
    put(b, at + 8, strAt, 8);
    at += 16;
    put(b, at, 0, 8);                     // DT_NULL
    put(b, at + 8, 0, 8);

    b.resize(total, '\0');
    std::copy(strtab.begin(), strtab.end(), b.begin() + static_cast<long>(strAt));
    return b;
}

// ─── a minimal PE32+ image ──────────────────────────────────────────────
//
// One section mapping RVA 0x1000 → file 0x400, an import directory and a
// delay-import directory inside it.
//
// WRITTEN AS PLAINLY AS POSSIBLE, and the breadcrumbs are not decoration.
// The first version used two `std::span` parameters and two lambdas that
// mutated a captured string through a captured cursor; it segfaulted on the
// macOS ARM64 runner and NOWHERE else — not under ASan+UBSan with clang 22 +
// libc++, not as a clang module on x86_64 Linux, not under gcc. Splitting the
// test proved the crash is HERE, in fixture code that touches no module at
// all, so the shapes went and the trace stayed: if it moves again, the log
// says which step.
// Off by default — 24 lines of stderr in every CI run forever is a poor
// trade for a crash that is currently fixed. `MCPP_TEST_TRACE=1` brings it
// back, which is what makes a recurrence one CI round to localise instead of
// the four this one cost.
void trace(const char* step, const std::string& b) {
    static const bool on = std::getenv("MCPP_TEST_TRACE") != nullptr;
    if (!on) return;
    std::fprintf(stderr, "[pe-fixture] %-14s size=%zu\n", step, b.size());
    std::fflush(stderr);
}

std::string pe_with_imports(const std::vector<std::string>& imports,
                            const std::vector<std::string>& delayImports) {
    const std::size_t   kNt      = 0x40;
    const std::size_t   kOptSize = 0xF0;      // 112 + 16 directories * 8
    const std::size_t   kSecAt   = kNt + 24 + kOptSize;
    const std::size_t   kRawAt   = 0x400;
    const std::uint32_t kSecVa   = 0x1000;
    const std::size_t   dirsAt   = kNt + 24 + 112;

    std::string b(kRawAt, '\0');
    trace("start", b);
    b[0] = 'M'; b[1] = 'Z';
    put(b, 0x3C, kNt, 4);
    b[kNt] = 'P'; b[kNt + 1] = 'E';                     // "PE\0\0"
    put(b, kNt + 4, 0x8664, 2);                          // Machine = AMD64
    put(b, kNt + 6, 1, 2);                               // NumberOfSections
    put(b, kNt + 20, kOptSize, 2);                       // SizeOfOptionalHeader
    put(b, kNt + 24, 0x20b, 2);                          // PE32+
    put(b, dirsAt - 4, 16, 4);                           // NumberOfRvaAndSizes
    trace("headers", b);

    // Section header: name, VirtualSize, VirtualAddress, SizeOfRawData,
    // PointerToRawData.
    const std::string secName = ".rdata";
    for (std::size_t i = 0; i < secName.size(); ++i) b[kSecAt + i] = secName[i];
    put(b, kSecAt + 8,  0x1000, 4);
    put(b, kSecAt + 12, kSecVa, 4);
    put(b, kSecAt + 16, 0x1000, 4);
    put(b, kSecAt + 20, kRawAt, 4);
    trace("section", b);

    // Names first, so the descriptors can point at them.
    std::vector<std::uint32_t> importNameRvas;
    std::vector<std::uint32_t> delayNameRvas;
    std::size_t cursor = kRawAt;
    for (std::size_t which = 0; which < 2; ++which) {
        const std::vector<std::string>& names = which == 0 ? imports : delayImports;
        for (std::size_t k = 0; k < names.size(); ++k) {
            const std::uint32_t rva =
                static_cast<std::uint32_t>(kSecVa + (cursor - kRawAt));
            const std::string& n = names[k];
            for (std::size_t i = 0; i < n.size(); ++i) {
                put(b, cursor, static_cast<unsigned char>(n[i]), 1);
                ++cursor;
            }
            put(b, cursor, 0, 1);
            ++cursor;
            if (which == 0) importNameRvas.push_back(rva);
            else            delayNameRvas.push_back(rva);
        }
    }
    trace("names", b);

    // Import descriptors (20 bytes each) + an all-zero terminator.
    cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
    const std::size_t importAt = cursor;
    for (std::size_t k = 0; k < importNameRvas.size(); ++k) {
        put(b, cursor + 0,  0x9000, 4);      // OriginalFirstThunk (nonzero)
        put(b, cursor + 12, importNameRvas[k], 4);   // Name
        put(b, cursor + 16, 0x9100, 4);      // FirstThunk (nonzero)
        cursor += 20;
    }
    for (std::size_t i = 0; i < 20; ++i) { put(b, cursor, 0, 1); ++cursor; }
    trace("imports", b);

    // Delay-import descriptors (32 bytes each). grAttrs bit 0 = the fields
    // are RVAs; without it a descriptor is the pre-VC7 address form and must
    // be skipped rather than misread.
    cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
    const std::size_t delayAt = cursor;
    for (std::size_t k = 0; k < delayNameRvas.size(); ++k) {
        put(b, cursor + 0, 1, 4);                    // grAttrs = dlattrRva
        put(b, cursor + 4, delayNameRvas[k], 4);     // rvaDLLName
        cursor += 32;
    }
    for (std::size_t i = 0; i < 32; ++i) { put(b, cursor, 0, 1); ++cursor; }
    trace("delay", b);

    if (!imports.empty()) {
        put(b, dirsAt + 1 * 8,
            static_cast<std::uint32_t>(kSecVa + (importAt - kRawAt)), 4);
        put(b, dirsAt + 1 * 8 + 4, 20 * (imports.size() + 1), 4);
    }
    if (!delayImports.empty()) {
        put(b, dirsAt + 13 * 8,
            static_cast<std::uint32_t>(kSecVa + (delayAt - kRawAt)), 4);
        put(b, dirsAt + 13 * 8 + 4, 32 * (delayImports.size() + 1), 4);
    }
    trace("directories", b);
    return b;
}

// The three names every PE test below builds an image around. A function, not
// a namespace-scope constant: a `std::vector<std::string>` at namespace scope
// in a test TU is a static initializer, and this file is already investigating
// one platform-specific crash.
std::vector<std::string> pe_imports()  { return {"KERNEL32.dll", "vcruntime140.dll"}; }
std::vector<std::string> pe_delayed()  { return {"dbghelp.dll"}; }

struct TempFile {
    std::filesystem::path path;
    TempFile(std::string_view tag, std::string_view bytes)
        : path(write_temp(tag, bytes)) {}
    ~TempFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

} // namespace

TEST(PackBinfmt, IdentifiesElfWithoutRunningIt) {
    std::array<std::string_view, 1> needed{"libc.so.6"};
    TempFile f{"elf", elf_with_needed(needed)};
    auto id = bf::identify(f.path);
    EXPECT_EQ(id.format, bf::Format::Elf);
    EXPECT_EQ(id.arch, "x86_64");
    EXPECT_TRUE(id.is64);
}

TEST(PackBinfmt, ReadsElfDtNeededThroughTheSegmentTable) {
    // DT_STRTAB is a virtual address, so the parser has to translate it via
    // PT_LOAD. Two entries, in file order.
    std::array<std::string_view, 2> needed{"libstdc++.so.6", "libcustom.so.1"};
    TempFile f{"elfneeded", elf_with_needed(needed)};
    auto names = bf::needed_names(f.path);
    ASSERT_TRUE(names.has_value()) << names.error();
    ASSERT_EQ(names->size(), 2u);
    EXPECT_EQ((*names)[0], "libstdc++.so.6");
    EXPECT_EQ((*names)[1], "libcustom.so.1");
}

TEST(PackBinfmt, AnElfWithNoDynamicSectionHasZeroDepsAndIsNotAnError) {
    // A fully static binary. "No dependencies" and "could not be read" must
    // not look alike — one is a packageable artifact and the other is a bug.
    auto bytes = elf_with_needed({});
    // Strip PT_DYNAMIC by zeroing its type field.
    put(bytes, 120, 0, 4);
    TempFile f{"elfstatic", bytes};
    auto names = bf::needed_names(f.path);
    ASSERT_TRUE(names.has_value()) << names.error();
    EXPECT_TRUE(names->empty());
}

// SPLIT INTO THREE ON PURPOSE. The combined version died with SIGSEGV on the
// macOS ARM64 runner and nowhere else, and a single test that builds a
// fixture, identifies it and parses it cannot say WHICH of the three it was.
// A test that cannot localise its own failure is a test that costs a CI round
// per hypothesis.
TEST(PackBinfmt, ThePeFixtureItselfIsWellFormed) {
    auto bytes = pe_with_imports(pe_imports(), pe_delayed());
    ASSERT_GT(bytes.size(), 0x400u);
    EXPECT_EQ(bytes.substr(0, 2), "MZ");
    EXPECT_EQ(bytes.substr(0x40, 4), std::string("PE\0\0", 4));
    // The names have to be IN the image, or every assertion below is about
    // the fixture rather than about the reader.
    for (auto want : {"KERNEL32.dll", "vcruntime140.dll", "dbghelp.dll"})
        EXPECT_NE(bytes.find(want), std::string::npos) << want;
}

TEST(PackBinfmt, IdentifiesPe) {
    TempFile f{"peid", pe_with_imports(pe_imports(), pe_delayed())};

    auto id = bf::identify(f.path);
    EXPECT_EQ(id.format, bf::Format::Pe);
    EXPECT_EQ(id.arch, "x86_64");
    EXPECT_TRUE(id.is64);
}

TEST(PackBinfmt, ReadsBothPeImportDirectories) {
    TempFile f{"peimp", pe_with_imports(pe_imports(), pe_delayed())};

    auto names = bf::needed_names(f.path);
    ASSERT_TRUE(names.has_value()) << names.error();
    // The DELAY-loaded one is a dependency too, and leaving it out is worse
    // than leaving out an ordinary import: a missing delay-load does not fail
    // at startup, it fails at the first call through it.
    EXPECT_EQ(names->size(), 3u);
    for (auto want : {"KERNEL32.dll", "vcruntime140.dll", "dbghelp.dll"}) {
        bool found = false;
        for (auto const& n : *names) found = found || n == want;
        EXPECT_TRUE(found) << want << " missing from the closure";
    }
}

TEST(PackBinfmt, ADosStubWithoutAPeSignatureIsNotAPe) {
    // "MZ" alone is a DOS executable. Calling it PE would send the caller
    // into a parser that cannot succeed, and the error would describe the
    // wrong thing.
    std::string b(0x100, '\0');
    b[0] = 'M'; b[1] = 'Z';
    put(b, 0x3C, 0x40, 4);          // points at zeros, not "PE\0\0"
    TempFile f{"dos", b};
    EXPECT_EQ(bf::identify(f.path).format, bf::Format::Unknown);
    EXPECT_FALSE(bf::needed_names(f.path).has_value());
}

TEST(PackBinfmt, TruncatedInputIsRejectedRatherThanRead) {
    // Malformed input is ordinary: a half-downloaded file, a text file named
    // `.exe`. Every read is bounds-checked, so the parser is total over it.
    for (std::size_t keep : {0u, 4u, 0x40u, 0x80u}) {
        auto bytes = pe_with_imports({"KERNEL32.dll"}, {});
        bytes.resize(keep);
        TempFile f{"trunc", bytes};
        auto names = bf::needed_names(f.path);
        if (names) EXPECT_TRUE(names->empty()) << "keep=" << keep;
    }
}

TEST(PackBinfmt, TheSystemPredicateKnowsWindowsFromTheToolset) {
    // Windows' own: shipping a private copy is a broken program, not a
    // heavier one.
    for (auto n : {"KERNEL32.dll", "kernel32.dll", "ucrtbase.dll", "ntdll.dll",
                   "api-ms-win-crt-runtime-l1-1-0.dll"})
        EXPECT_TRUE(bf::is_system_lib(bf::Format::Pe, n)) << n;

    // NOT Windows' own. These belong to the TOOLSET, and whether they travel
    // is `cxx_runtime`'s decision — the one call this predicate must not make
    // on its behalf.
    for (auto n : {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                   "libwinpthread-1.dll", "libstdc++-6.dll"})
        EXPECT_FALSE(bf::is_system_lib(bf::Format::Pe, n)) << n;

    // ELF keeps the manylinux allow-list it always had.
    EXPECT_TRUE(bf::is_system_lib(bf::Format::Elf, "libstdc++.so.6"));
    EXPECT_TRUE(bf::is_system_lib(bf::Format::Elf, "ld-linux-x86-64.so.2"));
    EXPECT_FALSE(bf::is_system_lib(bf::Format::Elf, "libcurl.so.4"));
}

// ─── the zip writer ──────────────────────────────────────────────────────

TEST(PackZip, Crc32MatchesTheKnownVectors) {
    // Self-consistency proves nothing here: an archive whose CRCs are wrong
    // extracts with a warning on some tools and silently on others.
    EXPECT_EQ(mcpp::pack::zip::crc32(""), 0u);
    EXPECT_EQ(mcpp::pack::zip::crc32("123456789"), 0xCBF43926u);
    EXPECT_EQ(mcpp::pack::zip::crc32(
        "The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(PackZip, WritesAReadableArchiveWithForwardSlashNames) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-zip-{}", std::chrono::steady_clock::now()
                                              .time_since_epoch().count());
    std::filesystem::create_directories(dir);
    std::ofstream{dir / "a.txt"} << "hello";
    std::ofstream{dir / "b.dll"} << "MZ-not-really";

    std::vector<mcpp::pack::zip::Entry> entries{
        {"pkg/a.txt", dir / "a.txt", false},
        {"pkg/b.dll", dir / "b.dll", false},
    };
    auto out = dir / "pkg.zip";
    auto r = mcpp::pack::zip::write(out, entries);
    ASSERT_TRUE(r.has_value()) << r.error();

    std::ifstream is(out, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(is)), {});
    ASSERT_GE(bytes.size(), 22u);
    EXPECT_EQ(bytes.substr(0, 4), std::string("PK\x03\x04", 4))
        << "no local file header signature";
    EXPECT_NE(bytes.find(std::string("PK\x01\x02", 4)), std::string::npos)
        << "no central directory";
    EXPECT_EQ(bytes.substr(bytes.size() - 22, 4), std::string("PK\x05\x06", 4))
        << "end-of-central-directory is not last (a trailing comment?)";
    // Names are stored verbatim; a backslash here is what makes an archive
    // written on Windows extract to one oddly-named file everywhere else.
    EXPECT_NE(bytes.find("pkg/a.txt"), std::string::npos);
    EXPECT_EQ(bytes.find("pkg\\a.txt"), std::string::npos);
    // Stored, not deflated: the payload appears literally.
    EXPECT_NE(bytes.find("hello"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(PackZip, TwoWritesOfTheSameInputAreByteIdentical) {
    // A published checksum only means something if the archive is a function
    // of its contents. Reading mtimes would make it a function of when.
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-zip-det-{}", std::chrono::steady_clock::now()
                                                  .time_since_epoch().count());
    std::filesystem::create_directories(dir);
    std::ofstream{dir / "x.bin"} << "payload";
    std::vector<mcpp::pack::zip::Entry> entries{{"p/x.bin", dir / "x.bin", true}};

    auto read = [](const std::filesystem::path& p) {
        std::ifstream is(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(is)), {});
    };
    ASSERT_TRUE(mcpp::pack::zip::write(dir / "one.zip", entries).has_value());
    ASSERT_TRUE(mcpp::pack::zip::write(dir / "two.zip", entries).has_value());
    EXPECT_EQ(read(dir / "one.zip"), read(dir / "two.zip"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
