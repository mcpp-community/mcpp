// mcpp.buildopt.keep.gcm — what mcpp knows of a GCC BMI (`.gcm`).
//
// A `.gcm` is not a function of the interface alone: it holds two wall-clock
// stamps, and its line maps move with a line inserted above a recorded
// location. Both defeat a cutoff by content, and both are properties of one
// compiler's file format. Everything that reads that format is in this file.
// The cutoff itself -- the question an edge asks, the build directory's
// statement and its record -- is mcpp.buildopt.keep and knows no format.

export module mcpp.buildopt.keep.gcm;

import std;
import mcpp.buildopt.keep.difference;

export namespace mcpp::buildopt::keep::gcm {

// Whether the bytes are a BMI of GCC's: the ELF file it writes.
bool is_format(std::string_view bytes);


// Are two BMIs equivalent for the purpose of "did this module's interface
// change?" — i.e. identical except for the wall clock GCC stamps into them.
//
// WHY THIS EXISTS. The `cxx_module` rule keeps the previous BMI, recompiles,
// and restores the old file when the new one has the same content, so ninja's
// restat sees an unchanged output and does NOT rebuild the importers. That
// mechanism was designed in 2026-05-12 and has NEVER ONCE FIRED, because GCC
// writes
//
//     buildtime: 2026/08/12 02:25:01 UTC
//     localtime: 2026/08/12 02:25:01 UTC
//
// INTO THE BMI CONTENT. Two compilations of identical source a second apart
// differ by exactly four bytes, so a plain `cmp` always reports "changed".
// Measured on this repository: touching a module with 46 importers and no
// content change cost 73.0 s and re-ran 180 edges — indistinguishable from a
// full rebuild.
//
// The earlier design note anticipated only that GCC would rewrite the FILE
// (mtime churn) and prescribed a content compare as the fix; it did not
// anticipate that the timestamp is part of the content, which is why the fix
// as written could not work.
//
// Deliberately NOT solved with SOURCE_DATE_EPOCH: that pins the epoch for the
// whole compilation and so changes what `__DATE__` and `__TIME__` expand to in
// USER code. Masking the two fields here changes what mcpp considers equal and
// nothing else.
//
// Conservative by construction: if the expected stamps are not found, or the
// two files disagree about where they are, this falls back to a strict
// comparison. It can report "different" for BMIs that are equivalent; it must
// never report "same" for BMIs that are not.
bool equivalent(const std::filesystem::path& a, const std::filesystem::path& b);

// WHAT TWO BMIS OF ONE MODULE DIFFER IN (mcpp#946, O2 and O6).
//
// `Same`: equal but for the two stamps, as `equivalent` answers.
// `LocationsOnly`: a GCC BMI whose line maps moved and nothing else. A `.gcm`
// is an ELF file; a line inserted where no recorded entity follows it in a
// way its own section records changes the section of the line maps
// (`.gnu.c++.olm`), and in the configuration section (`.gnu.c++.cfg`) its
// checksum and the module's (bytes 0-3 and 8-11; measured with gcc 16.1.0,
// the rest of the section is equal). An importer of the previous BMI then
// reads the previous lines: its diagnostics, debug information and coverage
// name them until it is compiled again for another reason.
//
// ONE CASE READS A LINE INTO THE PROGRAM, AND IT IS EXCLUDED. A template body
// that calls `__builtin_LINE()` or `std::source_location::current()` is
// evaluated where it is instantiated, from the line maps: measured, a line
// inserted before `template<class T> constexpr unsigned f(T) { return
// __builtin_LINE(); }` changed `.olm` and `.cfg` alone, and the importer's
// constant from 3 to 4. Such a body names the builtin in the BMI
// (`__builtin_LINE`, `__builtin_source_location`; the second is what
// `source_location::current()` leaves under `import std`), so a BMI that
// holds either name is compared exactly. A non-template inline function is
// folded where it is parsed and its own section changes.
Difference difference(const std::filesystem::path& a, const std::filesystem::path& b);

} // namespace mcpp::buildopt::keep::gcm

namespace mcpp::buildopt::keep::gcm {

bool is_format(std::string_view bytes) {
    return bytes.size() >= 52 && bytes.substr(0, 4) == "\x7f" "ELF";
}


namespace {

// GCC writes the stamp as `<prefix>YYYY/MM/DD HH:MM:SS <zone>` and ends it with
// a NUL. `buildtime:` is in UTC; `localtime:` carries the host's zone
// abbreviation, which is `UTC` only on a UTC host (measured with gcc 16.1.0:
// `CST` under Asia/Shanghai, `-03` under America/Sao_Paulo). A stamp that
// admitted ` UTC` alone left the cutoff without effect on every other host
// (mcpp#946, O1).
constexpr std::size_t kStampDateLen = std::string_view("2026/08/12 02:25:01 ").size();
constexpr std::size_t kMaxZoneLen = 8;

// The length of the stamp that starts `v`, zone included, or 0 when `v` does
// not start with one. The zone is 1 to 8 characters of a zone abbreviation
// and is followed by the terminator.
std::size_t stamp_length(std::string_view v) {
    if (v.size() < kStampDateLen + 2) return 0;
    auto digit = [&](std::size_t i) { return v[i] >= '0' && v[i] <= '9'; };
    const bool date = digit(0) && digit(1) && digit(2) && digit(3) && v[4] == '/'
        && digit(5) && digit(6) && v[7] == '/'
        && digit(8) && digit(9) && v[10] == ' '
        && digit(11) && digit(12) && v[13] == ':'
        && digit(14) && digit(15) && v[16] == ':'
        && digit(17) && digit(18) && v[19] == ' ';
    if (!date) return 0;
    std::size_t zone = 0;
    for (; kStampDateLen + zone < v.size(); ++zone) {
        const char c = v[kStampDateLen + zone];
        if (c == '\0') return zone == 0 ? 0 : kStampDateLen + zone;
        const bool abbreviation = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '+' || c == '-';
        if (!abbreviation || zone == kMaxZoneLen) return 0;
    }
    return 0;
}

// GCC writes exactly one `buildtime:` and one `localtime:` into a BMI header —
// verified across BMIs from 10 KiB to 645 KiB, always 2. Anything beyond that
// came from somewhere else (a string literal in user code that happens to look
// like a stamp), and masking it would hide a REAL difference. Finding more than
// this many makes the comparison fall back to strict equality.
constexpr std::size_t kMaxStampSpans = 2;

// Byte spans to ignore, in ascending order. Only spans whose payload actually
// looks like a timestamp are masked — a prefix that happens to appear in some
// other position is left to compare strictly.
std::vector<std::pair<std::size_t, std::size_t>> stamp_spans(std::string_view data) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::string_view prefix : {"buildtime: ", "localtime: "}) {
        for (std::size_t at = data.find(prefix); at != std::string_view::npos;
             at = data.find(prefix, at + 1)) {
            const auto start = at + prefix.size();
            const auto length = stamp_length(data.substr(start));
            if (length == 0) continue;
            spans.emplace_back(start, start + length);
        }
    }
    std::ranges::sort(spans);
    return spans;
}

std::optional<std::string> read_all(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

bool equivalent(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto da = read_all(a);
    auto db = read_all(b);
    if (!da || !db) return false;
    // Two BMIs of one host carry stamps of one width, so equivalent BMIs have
    // equal size. A size difference is read as a real difference.
    if (da->size() != db->size()) return false;

    const auto sa = stamp_spans(*da);
    const auto sb = stamp_spans(*db);
    // Disagreement about WHERE the stamps are is itself a structural
    // difference; fall back to strict equality rather than guessing.
    if (sa != sb) return *da == *db;
    if (sa.empty() || sa.size() > kMaxStampSpans) return *da == *db;

    std::size_t cursor = 0;
    for (const auto& [start, end] : sa) {
        if (start > cursor
            && std::memcmp(da->data() + cursor, db->data() + cursor, start - cursor) != 0)
            return false;
        cursor = end;
    }
    return cursor >= da->size()
        || std::memcmp(da->data() + cursor, db->data() + cursor, da->size() - cursor) == 0;
}

namespace {

std::uint32_t le32(std::string_view d, std::size_t at) {
    return std::uint32_t(std::uint8_t(d[at])) | std::uint32_t(std::uint8_t(d[at + 1])) << 8
         | std::uint32_t(std::uint8_t(d[at + 2])) << 16 | std::uint32_t(std::uint8_t(d[at + 3])) << 24;
}
std::uint16_t le16(std::string_view d, std::size_t at) {
    return std::uint16_t(std::uint8_t(d[at]) | std::uint8_t(d[at + 1]) << 8);
}

// The byte spans of a GCC BMI that hold locations alone: the line-map section
// and the two checksums of the configuration section. Empty when the file is
// not the ELF32 little-endian file GCC writes, or has neither section.
std::vector<std::pair<std::size_t, std::size_t>> location_spans(std::string_view d) {
    constexpr std::size_t kHeader = 52, kSection = 40;
    if (d.size() < kHeader || d.substr(0, 4) != "\x7f" "ELF" || d[4] != 1 || d[5] != 1) return {};
    const std::size_t shoff = le32(d, 0x20);
    std::size_t count = le16(d, 0x30), names = le16(d, 0x32);
    if (le16(d, 0x2e) != kSection || shoff + kSection > d.size()) return {};
    // GCC states a count or a name-table index that does not fit the header
    // in the first section's fields, as ELF defines.
    if (count == 0) count = le32(d, shoff + 20);
    if (names == 0xffff) names = le32(d, shoff + 24);
    if (count == 0 || names >= count || shoff + count * kSection > d.size()) return {};
    const std::size_t strOff = le32(d, shoff + names * kSection + 16);
    const std::size_t strLen = le32(d, shoff + names * kSection + 20);
    if (strOff + strLen > d.size()) return {};
    const auto table = d.substr(strOff, strLen);
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::size_t i = 1; i < count; ++i) {
        const auto at = shoff + i * kSection;
        const std::size_t name = le32(d, at), off = le32(d, at + 16), len = le32(d, at + 20);
        if (name >= table.size() || off + len > d.size()) return {};
        const auto text = table.substr(name, table.find('\0', name) - name);
        if (text == ".gnu.c++.olm") spans.emplace_back(off, off + len);
        if (text == ".gnu.c++.cfg" && len >= 12) {
            spans.emplace_back(off, off + 4);
            spans.emplace_back(off + 8, off + 12);
        }
    }
    return spans;
}

bool equal_outside(std::string_view a, std::string_view b,
                   const std::vector<std::pair<std::size_t, std::size_t>>& spans) {
    std::size_t cursor = 0;
    for (const auto& [start, end] : spans) {
        if (start > cursor && a.substr(cursor, start - cursor) != b.substr(cursor, start - cursor))
            return false;
        cursor = std::max(cursor, end);
    }
    return cursor >= a.size() || a.substr(cursor) == b.substr(cursor);
}

} // namespace

Difference difference(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (gcm::equivalent(a, b)) return Difference::Same;
    auto da = read_all(a);
    auto db = read_all(b);
    if (!da || !db || da->size() != db->size()) return Difference::Changed;
    for (std::string_view reads : {"__builtin_LINE", "__builtin_source_location"})
        if (da->find(reads) != std::string::npos || db->find(reads) != std::string::npos)
            return Difference::Changed;
    auto spans = location_spans(*da);
    if (spans.empty() || spans != location_spans(*db)) return Difference::Changed;
    const auto sa = stamp_spans(*da);
    if (sa != stamp_spans(*db) || sa.size() > kMaxStampSpans) return Difference::Changed;
    spans.insert(spans.end(), sa.begin(), sa.end());
    std::ranges::sort(spans);
    return equal_outside(*da, *db, spans) ? Difference::LocationsOnly : Difference::Changed;
}

} // namespace mcpp::buildopt::keep::gcm
