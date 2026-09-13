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

// Big-endian counterpart, for the Mach-O fixtures below: a FAT header is
// always big-endian on disk, and one thin fixture is deliberately built with
// a byte-swapped (CIGAM) magic to exercise that leg of the reader.
void put_be(std::string& b, std::size_t at, std::uint64_t v, std::size_t width) {
    if (b.size() < at + width) b.resize(at + width, '\0');
    for (std::size_t i = 0; i < width; ++i)
        b[at + width - 1 - i] = static_cast<char>((v >> (8 * i)) & 0xFF);
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

// ─── minimal Mach-O objects (thin and fat) ───────────────────────────────
//
// Same reasoning as the ELF and PE fixtures above: a real Mach-O needs a
// macOS toolchain to produce, which is the dependency this reader exists to
// remove. Hand-built headers make "found these two names at these load
// commands, in this order" exact.

// mach/machine.h. Kept local to the fixture builder rather than imported
// from `mcpp.pack.binfmt`, which does not export them (a fixture is allowed
// to know the format's own constants; a caller of the module should not have
// to).
constexpr std::uint32_t kCpuTypeX86_64 = 0x01000007;
constexpr std::uint32_t kCpuTypeArm64  = 0x0100000c;

constexpr std::uint32_t kLcLoadDylib      = 0x0000000c;
constexpr std::uint32_t kLcLoadWeakDylib  = 0x80000018;
constexpr std::uint32_t kLcRpath          = 0x8000001c;

// One load command to bake into a fixture: `cmd` plus the string it carries.
// `structSize` is 24 for a dylib_command (name is the first field, after
// timestamp/current_version/compatibility_version at offsets 12/16/20 sit
// before the string) and 12 for an rpath_command (path is the only field
// after cmd/cmdsize).
struct Lc { std::uint32_t cmd; std::size_t structSize; std::string str; };

constexpr std::size_t kDylibStructSize = 24;
constexpr std::size_t kRpathStructSize = 12;

// A thin Mach-O object: `is64` selects `mach_header`/`mach_header_64`,
// `bigEndian` selects the plain or the CIGAM (byte-swapped) magic, and every
// integer in the header and its load commands is written in the byte order
// `bigEndian` names — exactly what a real byte-swapped object would contain,
// and what `macho_thin_needed` has to undo to read it.
std::string macho_thin(bool is64, bool bigEndian, std::uint32_t cputype,
                       const std::vector<Lc>& cmds)
{
    auto putN = [&](std::string& b, std::size_t at, std::uint64_t v, std::size_t w) {
        if (bigEndian) put_be(b, at, v, w); else put(b, at, v, w);
    };

    std::string magic;
    if (is64) magic = bigEndian ? std::string("\xfe\xed\xfa\xcf", 4)
                                : std::string("\xcf\xfa\xed\xfe", 4);
    else      magic = bigEndian ? std::string("\xfe\xed\xfa\xce", 4)
                                : std::string("\xce\xfa\xed\xfe", 4);

    const std::size_t headerSize = is64 ? 32 : 28;
    std::string b(headerSize, '\0');
    std::copy(magic.begin(), magic.end(), b.begin());
    putN(b, 4,  cputype, 4);       // cputype
    putN(b, 8,  0, 4);             // cpusubtype
    putN(b, 12, 2, 4);             // filetype = MH_EXECUTE
    putN(b, 16, cmds.size(), 4);   // ncmds
    putN(b, 20, 0, 4);             // sizeofcmds, filled in below
    putN(b, 24, 0, 4);             // flags
    if (is64) putN(b, 28, 0, 4);   // reserved

    std::size_t cursor = headerSize;
    for (auto const& lc : cmds) {
        const std::size_t rawSize = lc.structSize + lc.str.size() + 1;
        // Padded to a 4-byte boundary, as a real linker's cmdsize is —
        // exercising that the reader trusts `cmdsize` to advance, not the
        // string's own length.
        const std::size_t cmdsize = (rawSize + 3) & ~std::size_t(3);
        b.resize(cursor + cmdsize, '\0');
        putN(b, cursor + 0, lc.cmd, 4);
        putN(b, cursor + 4, cmdsize, 4);
        putN(b, cursor + 8, lc.structSize, 4);   // lc_str offset from cmd start
        std::copy(lc.str.begin(), lc.str.end(), b.begin() + static_cast<long>(cursor + lc.structSize));
        cursor += cmdsize;
    }
    putN(b, 20, cursor - headerSize, 4);   // sizeofcmds, now that it is known
    return b;
}

// A fat (universal) Mach-O: FAT_MAGIC plus one 20-byte `fat_arch` entry per
// slice, ALWAYS big-endian regardless of what the slices themselves are.
std::string macho_fat(const std::vector<std::pair<std::uint32_t, std::string>>& slices)
{
    const std::size_t headerSize = 8 + slices.size() * 20;
    std::vector<std::size_t> sliceOffsets;
    std::size_t cursor = headerSize;
    for (auto const& [cputype, bytes] : slices) {
        sliceOffsets.push_back(cursor);
        cursor += bytes.size();
    }

    std::string b(cursor, '\0');
    put_be(b, 0, 0xcafebabe, 4);            // FAT_MAGIC
    put_be(b, 4, slices.size(), 4);         // nfat_arch
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const std::size_t at = 8 + i * 20;
        put_be(b, at + 0,  slices[i].first, 4);        // cputype
        put_be(b, at + 4,  0, 4);                      // cpusubtype
        put_be(b, at + 8,  sliceOffsets[i], 4);         // offset
        put_be(b, at + 12, slices[i].second.size(), 4); // size
        put_be(b, at + 16, 0, 4);                       // align
    }
    for (std::size_t i = 0; i < slices.size(); ++i)
        std::copy(slices[i].second.begin(), slices[i].second.end(),
                  b.begin() + static_cast<long>(sliceOffsets[i]));
    return b;
}

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

TEST(PackBinfmt, AGarbageELfanewIsRejectedAndDoesNotThrow) {
    // An "MZ" file whose `e_lfanew` points past the end is ordinary malformed
    // input: a truncated download, a DOS stub, a text file named `.exe`.
    //
    // This crashed. `std::string_view::substr` THROWS `std::out_of_range` when
    // `pos > size()`, and the offset comes straight out of the file — so
    // `identify()`, which is documented as never throwing, terminated the
    // process instead of answering Unknown. Bounds-checked comparison now.
    for (std::uint32_t lfanew : {0xFFFFFFFFu, 0x7FFFFFFFu, 0x10000u, 0x101u}) {
        std::string b(0x100, '\0');
        b[0] = 'M'; b[1] = 'Z';
        put(b, 0x3C, lfanew, 4);
        TempFile f{"mzjunk", b};
        EXPECT_NO_THROW({
            EXPECT_EQ(bf::identify(f.path).format, bf::Format::Unknown)
                << "e_lfanew=" << lfanew;
            EXPECT_FALSE(bf::needed_names(f.path).has_value());
        }) << "e_lfanew=" << lfanew;
    }
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

// ─── Mach-O: #630 §4 — the reader `needed_names` used to refuse ─────────

TEST(PackBinfmt, IdentifiesThinAndFatMachOWithoutRunningIt) {
    TempFile thin{"macho-thin",
        macho_thin(/*is64=*/true, /*bigEndian=*/false, kCpuTypeArm64,
                  {{kLcLoadDylib, kDylibStructSize, "/usr/lib/libSystem.B.dylib"}})};
    EXPECT_EQ(bf::identify(thin.path).format, bf::Format::MachO);

    TempFile fat{"macho-fat",
        macho_fat({{kCpuTypeX86_64, macho_thin(true, false, kCpuTypeX86_64, {})},
                   {kCpuTypeArm64,  macho_thin(true, false, kCpuTypeArm64, {})}})};
    EXPECT_EQ(bf::identify(fat.path).format, bf::Format::MachO);
}

TEST(PackBinfmt, ThinMachOReadsNamesAndRpathsInLoadCommandOrder) {
    // Two LC_LOAD_DYLIB, one LC_LOAD_WEAK_DYLIB, two LC_RPATH — the record's
    // §4.3 fixture shape, in one file.
    TempFile f{"macho-thin-arm64",
        macho_thin(true, false, kCpuTypeArm64, {
            {kLcLoadDylib,     kDylibStructSize, "@rpath/libfoo.dylib"},
            {kLcLoadDylib,     kDylibStructSize, "/usr/lib/libSystem.B.dylib"},
            {kLcLoadWeakDylib, kDylibStructSize, "@rpath/libbar.dylib"},
            {kLcRpath,         kRpathStructSize, "@executable_path/../Frameworks"},
            {kLcRpath,         kRpathStructSize, "@loader_path/../lib"},
        })};

    auto r = bf::macho_needed(f.path, "aarch64");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->names, (std::vector<std::string>{
        "@rpath/libfoo.dylib", "/usr/lib/libSystem.B.dylib", "@rpath/libbar.dylib"}));
    EXPECT_EQ(r->rpaths, (std::vector<std::string>{
        "@executable_path/../Frameworks", "@loader_path/../lib"}));

    // `needed_names` completes for Mach-O now: names alone, through the same
    // dispatcher ELF and PE already go through.
    auto names = bf::needed_names(f.path);
    ASSERT_TRUE(names.has_value()) << names.error();
    EXPECT_EQ(*names, r->names);
}

TEST(PackBinfmt, AWrongArchRequestOnAThinFileStillReadsIt) {
    // There is only one slice in a thin file; refusing to read it because
    // the caller asked for a different architecture would be wrong — the
    // caller already knows what it built, and mcpp never builds a thin
    // Mach-O whose OWN cputype disagrees with the triple that produced it.
    TempFile f{"macho-thin-wrongarch",
        macho_thin(true, false, kCpuTypeArm64,
                  {{kLcLoadDylib, kDylibStructSize, "@rpath/libfoo.dylib"}})};
    auto r = bf::macho_needed(f.path, "x86_64");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->names, (std::vector<std::string>{"@rpath/libfoo.dylib"}));
}

TEST(PackBinfmt, FatMachOReadsOnlyTheRequestedSlice) {
    auto x64 = macho_thin(true, false, kCpuTypeX86_64,
        {{kLcLoadDylib, kDylibStructSize, "onlyintel.dylib"}});
    auto arm = macho_thin(true, false, kCpuTypeArm64,
        {{kLcLoadDylib, kDylibStructSize, "onlyarm.dylib"}});
    TempFile fat{"macho-fat-differing",
        macho_fat({{kCpuTypeX86_64, x64}, {kCpuTypeArm64, arm}})};

    auto arm64Read = bf::macho_needed(fat.path, "aarch64");
    ASSERT_TRUE(arm64Read.has_value()) << arm64Read.error();
    EXPECT_EQ(arm64Read->names, (std::vector<std::string>{"onlyarm.dylib"}));
    // A name present only in the OTHER slice must be absent — the whole
    // point of selecting by architecture rather than reading both.
    EXPECT_EQ(std::ranges::find(arm64Read->names, "onlyintel.dylib"),
              arm64Read->names.end());

    auto x64Read = bf::macho_needed(fat.path, "x86_64");
    ASSERT_TRUE(x64Read.has_value()) << x64Read.error();
    EXPECT_EQ(x64Read->names, (std::vector<std::string>{"onlyintel.dylib"}));
}

TEST(PackBinfmt, ABigEndianMagicThinMachOIsReadCorrectly) {
    // The CIGAM leg: every integer in the header and its load commands is
    // written big-endian, which `macho_thin_needed` has to detect from the
    // magic alone and undo.
    TempFile f{"macho-thin-be",
        macho_thin(/*is64=*/true, /*bigEndian=*/true, kCpuTypeArm64,
                  {{kLcLoadDylib, kDylibStructSize, "/usr/lib/libSystem.B.dylib"},
                   {kLcRpath,     kRpathStructSize, "@executable_path/../lib"}})};
    auto r = bf::macho_needed(f.path, {});
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->names, (std::vector<std::string>{"/usr/lib/libSystem.B.dylib"}));
    EXPECT_EQ(r->rpaths, (std::vector<std::string>{"@executable_path/../lib"}));
}

TEST(PackBinfmt, TheSystemPredicateKnowsMachOsOwnRoots) {
    EXPECT_TRUE(bf::is_system_lib(bf::Format::MachO, "/usr/lib/libSystem.B.dylib"));
    EXPECT_TRUE(bf::is_system_lib(bf::Format::MachO,
        "/System/Library/Frameworks/Foundation.framework/Foundation"));
    // Not yet resolved to a file -- `@rpath` names a search, not a root.
    EXPECT_FALSE(bf::is_system_lib(bf::Format::MachO, "@rpath/libc++.1.dylib"));
}

TEST(PackBinfmt, ResolveMachoNamesTriesRpathsInOrderAndReportsUnresolved) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-macho-resolve-{}",
                           std::chrono::steady_clock::now().time_since_epoch().count());
    auto bin = dir / "bin";
    auto first = dir / "first";
    auto second = dir / "second";
    std::filesystem::create_directories(bin);
    std::filesystem::create_directories(first);
    std::filesystem::create_directories(second);
    // The SAME leaf name in both candidate directories, so a resolution that
    // ignores rpath order cannot be told apart from one that respects it --
    // only the CONTENT of which file won can.
    std::ofstream{first / "dup.dylib"} << "first";
    std::ofstream{second / "dup.dylib"} << "second";
    std::ofstream{first / "onlysecond-decoy.dylib"} << "unused";

    std::vector<std::string> names{
        "@rpath/dup.dylib",
        "@rpath/nowhere.dylib",
    };
    std::vector<std::string> rpaths{
        "@executable_path/../first",
        "@executable_path/../second",
    };

    auto resolved = bf::resolve_macho_names(names, rpaths, bin, bin);
    ASSERT_EQ(resolved.size(), 2u);

    EXPECT_EQ(resolved[0].name, "@rpath/dup.dylib");
    EXPECT_FALSE(resolved[0].unresolved);
    // `lexically_normal`: the resolver joins `@executable_path/../first` onto
    // `bin` without collapsing the `..` itself (that is the filesystem's job,
    // at `exists()`), so the raw and the hand-built path differ textually
    // while naming the same file.
    EXPECT_EQ(resolved[0].path.lexically_normal(), (first / "dup.dylib").lexically_normal())
        << "the FIRST rpath entry must win when both would resolve";

    // Reported, not skipped: the vector still carries an entry for the name
    // that resolved nowhere.
    EXPECT_EQ(resolved[1].name, "@rpath/nowhere.dylib");
    EXPECT_TRUE(resolved[1].unresolved);
    EXPECT_TRUE(resolved[1].path.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(PackBinfmt, ResolveMachoNamesSubstitutesExecutableAndLoaderPath) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-macho-resolve2-{}",
                           std::chrono::steady_clock::now().time_since_epoch().count());
    auto exeDir = dir / "bin";
    auto loaderDir = dir / "lib" / "plugins";
    std::filesystem::create_directories(exeDir);
    std::filesystem::create_directories(loaderDir);
    std::ofstream{exeDir / "libbeside.dylib"} << "x";
    std::ofstream{loaderDir / "libplugin.dylib"} << "x";

    std::vector<std::string> names{
        "@executable_path/libbeside.dylib",
        "@loader_path/libplugin.dylib",
    };
    auto resolved = bf::resolve_macho_names(names, {}, exeDir, loaderDir);
    ASSERT_EQ(resolved.size(), 2u);
    EXPECT_EQ(resolved[0].path, exeDir / "libbeside.dylib");
    EXPECT_FALSE(resolved[0].unresolved);
    EXPECT_EQ(resolved[1].path, loaderDir / "libplugin.dylib");
    EXPECT_FALSE(resolved[1].unresolved);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
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
