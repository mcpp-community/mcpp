#include <gtest/gtest.h>

import std;
import mcpp.pack.relocate;
import mcpp.pack.strip;

// mcpp.pack.relocate / mcpp.pack.strip — the two halves of "the build machine
// does not travel", tested where CI cannot otherwise reach them.
//
// WHY SYNTHETIC ELF BYTES RATHER THAN A COMPILED .so
//
// The e2e suite already packs a real library and reads the result back, so the
// x86_64/little-endian path has an end-to-end judge. What it CANNOT reach is
// ELF32 and big-endian: no CI job produces one, and `mcpp pack --target` will
// hand exactly those to this code the first time somebody packages for a
// 32-bit or MIPS/PowerPC target. A wrong width or a wrong byte order there
// does not fail — it writes a corrupt dynamic section, which is the failure
// class this whole area exists to remove.
//
// The images below are the smallest thing that is still an ELF for the purpose
// of this question: a header, a PT_LOAD so DT_STRTAB can be mapped, a
// PT_DYNAMIC, and a string table. They are read back by a parser written here,
// deliberately NOT by the module under test — checking an editor with its own
// reader proves only that it is self-consistent.

namespace {

struct Dyn { std::uint64_t tag, val; };

// A minimal but well-formed ELF image carrying `entries`.
struct SyntheticElf {
    bool wide;        // true = ELF64
    bool little;
    std::string bytes;

    std::size_t ptrW()  const { return wide ? 8 : 4; }
    std::size_t slotW() const { return ptrW() * 2; }
    std::size_t dynOff = 0;
    std::size_t strOff = 0;
};

void put(std::string& b, std::size_t off, std::size_t width,
         std::uint64_t v, bool little)
{
    for (std::size_t i = 0; i < width; ++i)
        b[off + (little ? i : width - 1 - i)] =
            static_cast<char>((v >> (8 * i)) & 0xFF);
}

std::uint64_t get(const std::string& b, std::size_t off, std::size_t width,
                  bool little)
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i)
        v |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(b[off + (little ? i : width - 1 - i)]))
             << (8 * i);
    return v;
}

// `strings` are laid out in order; a Dyn whose tag is in `stringTags` has its
// value replaced by that string's offset.
SyntheticElf make_elf(bool wide, bool little,
                      std::vector<Dyn> entries,
                      const std::vector<std::string>& strings,
                      const std::vector<std::size_t>& stringSlots)
{
    SyntheticElf e{ wide, little, {}, };
    const std::size_t ehSize   = wide ? 64 : 52;
    const std::size_t phEnt    = wide ? 56 : 32;
    const std::size_t phNum    = 2;
    const std::size_t phOff    = ehSize;
    const std::size_t dynOff   = phOff + phEnt * phNum;
    const std::size_t slotW    = (wide ? 8 : 4) * 2;
    const std::size_t dynSize  = slotW * entries.size();
    const std::size_t strOff   = dynOff + dynSize;

    std::string strtab;
    std::vector<std::size_t> strAt;
    strtab.push_back('\0');                       // index 0 is the empty string
    for (auto const& s : strings) { strAt.push_back(strtab.size()); strtab += s; strtab.push_back('\0'); }

    e.bytes.assign(strOff + strtab.size(), '\0');
    e.dynOff = dynOff;
    e.strOff = strOff;

    // ── ELF header ────────────────────────────────────────────────────
    e.bytes[0] = 0x7F; e.bytes[1] = 'E'; e.bytes[2] = 'L'; e.bytes[3] = 'F';
    e.bytes[4] = static_cast<char>(wide ? 2 : 1);
    e.bytes[5] = static_cast<char>(little ? 1 : 2);
    e.bytes[6] = 1;
    put(e.bytes, 16, 2, 3, little);                       // e_type = ET_DYN
    put(e.bytes, 18, 2, wide ? 62 : 3, little);           // e_machine
    put(e.bytes, wide ? 0x20 : 0x1C, wide ? 8 : 4, phOff, little);
    put(e.bytes, wide ? 0x36 : 0x2A, 2, phEnt, little);
    put(e.bytes, wide ? 0x38 : 0x2C, 2, phNum, little);

    // ── PT_LOAD covering the whole image, vaddr == file offset ────────
    const std::size_t poOff = wide ? 0x08 : 0x04;
    const std::size_t pvOff = wide ? 0x10 : 0x08;
    const std::size_t pfOff = wide ? 0x20 : 0x10;
    put(e.bytes, phOff, 4, 1, little);                    // PT_LOAD
    put(e.bytes, phOff + poOff, wide ? 8 : 4, 0, little);
    put(e.bytes, phOff + pvOff, wide ? 8 : 4, 0, little);
    put(e.bytes, phOff + pfOff, wide ? 8 : 4, e.bytes.size(), little);

    // ── PT_DYNAMIC ────────────────────────────────────────────────────
    put(e.bytes, phOff + phEnt, 4, 2, little);            // PT_DYNAMIC
    put(e.bytes, phOff + phEnt + poOff, wide ? 8 : 4, dynOff, little);
    put(e.bytes, phOff + phEnt + pfOff, wide ? 8 : 4, dynSize, little);

    // ── the dynamic array ─────────────────────────────────────────────
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto value = entries[i].val;
        for (std::size_t k = 0; k < stringSlots.size(); ++k)
            if (stringSlots[k] == i) value = strAt[k];
        put(e.bytes, dynOff + i * slotW, wide ? 8 : 4, entries[i].tag, little);
        put(e.bytes, dynOff + i * slotW + (wide ? 8 : 4), wide ? 8 : 4, value, little);
    }
    // DT_STRTAB's value is a VADDR, and vaddr == file offset here by construction.
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].tag == 5)
            put(e.bytes, dynOff + i * slotW + (wide ? 8 : 4), wide ? 8 : 4, strOff, little);

    std::memcpy(e.bytes.data() + strOff, strtab.data(), strtab.size());
    return e;
}

// An independent reader: what tags does this image's dynamic array carry?
std::vector<std::uint64_t> read_tags(const std::string& bytes, std::size_t dynOff,
                                     bool wide, bool little)
{
    const std::size_t ptrW = wide ? 8 : 4;
    std::vector<std::uint64_t> tags;
    for (std::size_t at = dynOff; at + ptrW * 2 <= bytes.size(); at += ptrW * 2) {
        auto tag = get(bytes, at, ptrW, little);
        tags.push_back(tag);
        if (tag == 0) break;
    }
    return tags;
}

std::filesystem::path write_temp(const std::string& bytes, std::string_view stem) {
    auto dir = std::filesystem::temp_directory_path() / "mcpp-relocate-test";
    std::filesystem::create_directories(dir);
    auto p = dir / std::string(stem);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return p;
}

constexpr std::uint64_t kNull = 0, kNeeded = 1, kStrtab = 5, kSoname = 14,
                        kRpath = 15, kRunpath = 29;

} // namespace

// ── the ELF editor ──────────────────────────────────────────────────────

TEST(PackRelocate, RemovesRunpathFromElf64LittleEndian) {
    auto e = make_elf(/*wide=*/true, /*little=*/true,
        { {kNeeded, 0}, {kStrtab, 0}, {kRunpath, 0}, {kSoname, 0}, {kNull, 0} },
        { "libc.so.6", "/home/builder/.mcpp/store/lib64", "libfoo.so.1" },
        { 0, 2, 3 });
    const auto before = e.bytes.size();
    auto path = write_temp(e.bytes, "elf64le.so");

    auto r = mcpp::pack::relocate::strip_search_paths(path);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(r->outcome, mcpp::pack::relocate::Outcome::Removed);
    ASSERT_EQ(r->paths.size(), 1u);
    // The report names what was dropped — a log line that says only "removed"
    // cannot tell a stale store path from `$ORIGIN`.
    EXPECT_EQ(r->paths[0], "/home/builder/.mcpp/store/lib64");

    std::ifstream in(path, std::ios::binary);
    std::string after{ std::istreambuf_iterator<char>(in), {} };
    // SAME LENGTH: the freed slot becomes DT_NULL padding, so nothing after
    // PT_DYNAMIC moves and no offset in the file needs fixing up.
    EXPECT_EQ(after.size(), before);

    auto tags = read_tags(after, e.dynOff, true, true);
    EXPECT_EQ(std::ranges::count(tags, kRunpath), 0);
    // The other entries survive, in order. Losing DT_SONAME here would make the
    // library unfindable by the name its consumers link against.
    EXPECT_EQ(std::ranges::count(tags, kNeeded), 1);
    EXPECT_EQ(std::ranges::count(tags, kSoname), 1);
    EXPECT_EQ(std::ranges::count(tags, kStrtab), 1);
}

TEST(PackRelocate, RemovesRpathFromElf32BigEndian) {
    // No CI job produces this shape, and `--target` can. A width or byte-order
    // mistake would corrupt the image rather than fail.
    auto e = make_elf(/*wide=*/false, /*little=*/false,
        { {kNeeded, 0}, {kStrtab, 0}, {kRpath, 0}, {kNull, 0} },
        { "libc.so.6", "/home/builder/store/lib" },
        { 0, 2 });
    const auto before = e.bytes.size();
    auto path = write_temp(e.bytes, "elf32be.so");

    auto r = mcpp::pack::relocate::strip_search_paths(path);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(r->outcome, mcpp::pack::relocate::Outcome::Removed);
    ASSERT_EQ(r->paths.size(), 1u);
    EXPECT_EQ(r->paths[0], "/home/builder/store/lib");

    std::ifstream in(path, std::ios::binary);
    std::string after{ std::istreambuf_iterator<char>(in), {} };
    EXPECT_EQ(after.size(), before);
    auto tags = read_tags(after, e.dynOff, false, false);
    EXPECT_EQ(std::ranges::count(tags, kRpath), 0);
    EXPECT_EQ(std::ranges::count(tags, kNeeded), 1);
}

TEST(PackRelocate, RemovesBothTagsWhenAnImageCarriesBoth) {
    // `Both` is what a linker driven with `--disable-new-dtags` after an
    // explicit -rpath can produce, and the loader honours DT_RUNPATH there.
    // Leaving either one behind leaves the defect behind.
    auto e = make_elf(true, true,
        { {kStrtab, 0}, {kRpath, 0}, {kRunpath, 0}, {kNeeded, 0}, {kNull, 0} },
        { "/a/store/lib", "/b/farm/lib", "libc.so.6" },
        { 1, 2, 3 });
    auto path = write_temp(e.bytes, "elfboth.so");

    auto r = mcpp::pack::relocate::strip_search_paths(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->outcome, mcpp::pack::relocate::Outcome::Removed);
    EXPECT_EQ(r->paths.size(), 2u);

    std::ifstream in(path, std::ios::binary);
    std::string after{ std::istreambuf_iterator<char>(in), {} };
    auto tags = read_tags(after, e.dynOff, true, true);
    EXPECT_EQ(std::ranges::count(tags, kRpath), 0);
    EXPECT_EQ(std::ranges::count(tags, kRunpath), 0);
}

TEST(PackRelocate, AlreadyCleanIsNotTheSameAsNotChecked) {
    auto e = make_elf(true, true,
        { {kNeeded, 0}, {kStrtab, 0}, {kNull, 0} }, { "libc.so.6" }, { 0 });
    auto path = write_temp(e.bytes, "elfclean.so");
    auto r = mcpp::pack::relocate::strip_search_paths(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->outcome, mcpp::pack::relocate::Outcome::NothingToDo);
    EXPECT_TRUE(r->paths.empty());
}

TEST(PackRelocate, AStaticArchiveIsNotApplicableRatherThanAnError) {
    // Every leg goes through this step, and a `.a` has no dynamic section to
    // begin with. Reporting that as a failure would make the caller branch on
    // the format before calling — which is the coupling this module removes.
    auto path = write_temp("!<arch>\n/               0           0     0     0       4         `\n",
                           "libfoo.a");
    auto r = mcpp::pack::relocate::strip_search_paths(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->outcome, mcpp::pack::relocate::Outcome::NotApplicable);
}

TEST(PackRelocate, AMissingFileIsAnError) {
    auto r = mcpp::pack::relocate::strip_search_paths(
        std::filesystem::temp_directory_path() / "mcpp-relocate-test" / "nope.so");
    EXPECT_FALSE(r.has_value());
}

// ── the strip table ─────────────────────────────────────────────────────

TEST(PackStrip, StaticArchiveKeepsItsSymbolIndex) {
    // THE MEASUREMENT BEHIND THIS TEST. `strip --strip-all` on a `.a`
    // removes the archive symbol index, and the consumer's link then fails
    // with `archive has no index; run ranlib to add one` — a message that
    // names neither strip nor the publisher. `--strip-debug` keeps it
    // (measured: 2988 -> 1244 bytes, and the consumer links and runs).
    auto args = mcpp::pack::strip_args(mcpp::pack::ArtifactShape::StaticArchive);
    EXPECT_NE(std::ranges::find(args, "--strip-debug"), args.end());
    EXPECT_EQ(std::ranges::find(args, "--strip-all"), args.end());
    EXPECT_EQ(std::ranges::find(args, "--strip-unneeded"), args.end());
    // Two identical packs should produce identical bytes; this is the one
    // place the packer rewrites an archive.
    EXPECT_NE(std::ranges::find(args, "--enable-deterministic-archives"), args.end());
}

TEST(PackStrip, SharedLibraryKeepsItsDynamicSymbols) {
    // `--strip-unneeded` removes `.symtab` and keeps `.dynsym`, which IS the
    // export list. `--strip-all` would also keep `.dynsym`, but dh_strip uses
    // the narrower flag and there is no reason to be broader than the
    // reference implementation of this exact decision.
    auto args = mcpp::pack::strip_args(mcpp::pack::ArtifactShape::SharedLibrary);
    EXPECT_NE(std::ranges::find(args, "--strip-unneeded"), args.end());
    EXPECT_EQ(std::ranges::find(args, "--strip-all"), args.end());
    EXPECT_EQ(std::ranges::find(args, "--strip-debug"), args.end());
}

TEST(PackStrip, ExecutableIsStrippedWhole) {
    auto args = mcpp::pack::strip_args(mcpp::pack::ArtifactShape::Executable);
    EXPECT_NE(std::ranges::find(args, "--strip-all"), args.end());
}

TEST(PackStrip, EveryShapeDropsCommentAndNote) {
    // dh_strip drops both from all three. `.note` is matched by exact name, so
    // `.note.gnu.build-id` survives — which is what `--add-gnu-debuglink` and
    // debuginfod pair with.
    for (auto shape : { mcpp::pack::ArtifactShape::Executable,
                        mcpp::pack::ArtifactShape::SharedLibrary,
                        mcpp::pack::ArtifactShape::StaticArchive }) {
        auto args = mcpp::pack::strip_args(shape);
        EXPECT_NE(std::ranges::find(args, "--remove-section=.comment"), args.end());
        EXPECT_NE(std::ranges::find(args, "--remove-section=.note"), args.end());
    }
}

TEST(PackStrip, WhetherStrippingAppliesIsAskedOfTheTargetNotTheCompiler) {
    // Keying this on the compiler is wrong in BOTH directions, and each
    // direction is a real configuration mcpp ships:
    //
    //   clang -> x86_64-windows-msvc   produces .pdb debug info; a
    //                                  compiler-keyed rule would try to strip
    //                                  in-band DWARF that is not there.
    //   Apple clang on macOS           ships no `llvm-strip`, so a
    //                                  compiler-keyed rule would REFUSE every
    //                                  `mcpp pack` on a Mac — for a format
    //                                  whose image carries a debug MAP and
    //                                  leaves the DWARF in the .o files.
    EXPECT_TRUE (mcpp::pack::debug_info_is_in_band("x86_64-linux-gnu"));
    EXPECT_TRUE (mcpp::pack::debug_info_is_in_band("aarch64-linux-musl"));
    EXPECT_TRUE (mcpp::pack::debug_info_is_in_band("x86_64-windows-gnu"));
    EXPECT_FALSE(mcpp::pack::debug_info_is_in_band("x86_64-windows-msvc"));
    EXPECT_FALSE(mcpp::pack::debug_info_is_in_band("aarch64-macos"));
    EXPECT_FALSE(mcpp::pack::debug_info_is_in_band("x86_64-macos"));
    // Segment-wise, not substring: mcpp has been bitten by a triple predicate
    // that answered on a substring before.
    EXPECT_TRUE(mcpp::pack::debug_info_is_in_band("macos64-linux-gnu"));
}

TEST(PackStrip, MsvcHasNothingInBandToRemove) {
    // PE/MSVC keeps debug information in a separate `.pdb`. An empty `strip`
    // tool there is the right answer, not a missing dependency — and the two
    // must not be conflated, or every other platform gets a silent no-op.
    auto path = write_temp("not an image", "notanimage.bin");
    mcpp::pack::StripTools tools{ {}, {}, /*inBandDebugInfo=*/false };
    auto r = mcpp::pack::strip_artifact(path, mcpp::pack::ArtifactShape::SharedLibrary,
                                        tools, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->outcome, mcpp::pack::StripOutcome::NotApplicable);
}

TEST(PackStrip, AMissingStripToolIsRefusedWhereDebugInfoIsInBand) {
    auto path = write_temp("not an image", "notanimage2.bin");
    mcpp::pack::StripTools tools{ {}, {}, /*inBandDebugInfo=*/true };
    auto r = mcpp::pack::strip_artifact(path, mcpp::pack::ArtifactShape::SharedLibrary,
                                        tools, {});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("no `strip`"), std::string::npos);
}
