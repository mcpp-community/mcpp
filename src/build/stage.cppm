// mcpp.build.stage — the staging primitive behind the ninja `stage_file` rule.
//
// Staging = publish a file the build cache owns (std BMI, std.o, a dep's
// runtime DLL) into the build directory where the compiler / loader expects
// it. This used to be a shelled-out copy per platform:
//
//   POSIX:   mkdir -p $(dirname $out) && cp -f $in $out
//   Windows: powershell -NoProfile -Command "Copy-Item -Force '$in' -Destination '$out'"
//
// Both overwrite the destination in place, unconditionally. On Windows that is
// a live hazard (#311): mcpp writes the staged std BMI path into
// compile_commands.json so clangd can resolve `import std;`, clangd then
// memory-maps that very file, and an in-place overwrite of a file with an open
// user-mapped section fails with error 1224 — the whole build reports
// "build failed" even when the bytes were already correct.
//
// The rules this module implements:
//
//   1. Never write bytes that are already there. Equivalence is decided by
//      COMPARING CONTENT, which is unconditionally correct — a destination
//      identical to the source needs no write, whatever the reason. Size alone
//      is only a heuristic and is available as `--verify size` for the paths
//      where the caller knows better (a std BMI is fingerprint-scoped: the
//      cache dir and the build dir share the fp that covers compiler identity,
//      target triple, stdlib, std source hash and the dialect flags). It is
//      NOT the default, because staging also carries .dll payloads whose PE
//      section padding makes "same size, different bytes" ordinary.
//      Skipping mirrors what the dep BMI cache has always done
//      (bmi_cache.cppm: "Existing project outputs are left untouched").
//   2. When we do write, write out of place and rename — atomic for readers,
//      and it survives a transient sharing violation (antivirus, indexer)
//      that an in-place overwrite would lose to.
//   3. Retry a few times, then fail with a diagnostic that names the file and
//      the likely holder. Never downgrade a real staging failure to a warning:
//      a stale or missing BMI turns into either a confusing
//      "module 'std' not found" or a silently mismatched link.
//
// Skipping deliberately does not touch the destination's timestamps AT ALL —
// not even to align them with the source. Measured on the ninja side: any mtime
// bump (to "now" or to the source's mtime) counts as "the command changed this
// output", so `restat = 1` no longer suppresses the downstream rebuild and
// every importer of the staged BMI recompiles for nothing. Leaving the mtime
// alone keeps the edge dirty — ninja re-runs this ~no-op on each build until
// the content actually changes — which is the cheap side of the trade
// (one process vs. recompiling the module graph).

export module mcpp.build.stage;

import std;

export namespace mcpp::build::stage {

// How hard to look before declaring the destination already-staged.
enum class Verify {
    Content,    // default: byte-for-byte compare — always correct
    Size,       // size match only (MCPP_STAGE_VERIFY=size / --verify size)
};

struct StageOptions {
    Verify                    verify  = Verify::Content;
    int                       retries = 3;                 // attempts after the first
    std::chrono::milliseconds backoff{100};                // ×3 per retry: 100/300/900ms
};

struct StageOutcome {
    bool copied = false;    // false = destination was already equivalent
};

struct StageError {
    std::string message;    // multi-line, already carries the `hint:` block
};

// Publish `src` at `dst`. See the module comment for the exact semantics.
std::expected<StageOutcome, StageError> stage_file(const std::filesystem::path& src,
                                                   const std::filesystem::path& dst,
                                                   const StageOptions& opts = {});

// Byte-for-byte comparison (exported for tests). False when either file is
// unreadable or the sizes differ.
bool same_content(const std::filesystem::path& a, const std::filesystem::path& b);

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
bool bmi_equivalent(const std::filesystem::path& a, const std::filesystem::path& b);

// Parse a --verify / MCPP_STAGE_VERIFY value. Unknown values fall back to the
// safe default (Content).
Verify parse_verify(std::string_view value);

} // namespace mcpp::build::stage

namespace mcpp::build::stage {

namespace {

std::filesystem::path temp_sibling(const std::filesystem::path& dst) {
    // Same directory as the destination so the rename stays on one filesystem.
    auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    auto name = dst.filename().string() + std::format(".tmp.{}", stamp);
    return dst.parent_path() / name;
}

// One full write attempt: temp+rename first, in-place overwrite as fallback.
// Returns an empty error_code on success; otherwise the most informative error
// (the in-place one — that's where 1224 / 32 shows up).
std::error_code write_once(const std::filesystem::path& src,
                           const std::filesystem::path& dst) {
    auto tmp = temp_sibling(dst);
    std::error_code ec;
    std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
        std::error_code rec;
        std::filesystem::rename(tmp, dst, rec);
        if (!rec) return {};
        // Renaming over a destination that another process holds mapped fails
        // too (the destination must be deletable) — fall through to the
        // in-place path, which wins when the holder allows writes but not
        // deletes.
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
    } else {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
    }

    std::error_code cec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, cec);
    if (!cec) return {};
    return cec;
}

std::string failure_message(const std::filesystem::path& src,
                            const std::filesystem::path& dst,
                            const std::error_code& ec) {
    // ninja runs staging with cwd = the build directory, so `$out` is relative.
    // Print it absolute: the reader has to go find (or unlock) this file.
    std::error_code aec;
    auto shown = std::filesystem::absolute(dst, aec);
    if (aec) shown = dst;
    return std::format(
        "cannot stage file into the build directory\n"
        "  file:  {}\n"
        "  from:  {}\n"
        "  os error: {} ({})\n"
        "hint: another process has this file memory-mapped, loaded or open.\n"
        "      The usual holder is clangd (mcpp writes staged BMI paths into\n"
        "      compile_commands.json so clangd can resolve `import std;`), an\n"
        "      editor/IDE indexer, antivirus, or — for a .dll — a still-running\n"
        "      program from a previous `mcpp run`. Close it or restart clangd,\n"
        "      then re-run the build.",
        shown.string(), src.string(), ec.value(), ec.message());
}

} // namespace

bool same_content(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec1, ec2;
    auto sa = std::filesystem::file_size(a, ec1);
    auto sb = std::filesystem::file_size(b, ec2);
    if (ec1 || ec2 || sa != sb) return false;

    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    constexpr std::size_t kChunk = 1u << 16;
    std::vector<char> ba(kChunk), bb(kChunk);
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(kChunk));
        fb.read(bb.data(), static_cast<std::streamsize>(kChunk));
        auto na = fa.gcount();
        auto nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) break;
        if (std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(na)) != 0)
            return false;
    }
    return true;
}

namespace {

// GCC writes the stamp as `<prefix>YYYY/MM/DD HH:MM:SS UTC`. Fixed width, so a
// match can be masked without re-parsing.
constexpr std::size_t kStampLen = std::string_view("2026/08/12 02:25:01 UTC").size();

bool looks_like_stamp(std::string_view v) {
    if (v.size() != kStampLen) return false;
    auto digit = [&](std::size_t i) { return v[i] >= '0' && v[i] <= '9'; };
    return digit(0) && digit(1) && digit(2) && digit(3) && v[4] == '/'
        && digit(5) && digit(6) && v[7] == '/'
        && digit(8) && digit(9) && v[10] == ' '
        && digit(11) && digit(12) && v[13] == ':'
        && digit(14) && digit(15) && v[16] == ':'
        && digit(17) && digit(18) && v.substr(19) == " UTC";
}

// Byte spans to ignore, in ascending order. Only spans whose payload actually
// looks like a timestamp are masked — a prefix that happens to appear in some
// other position is left to compare strictly.
std::vector<std::pair<std::size_t, std::size_t>> stamp_spans(std::string_view data) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::string_view prefix : {"buildtime: ", "localtime: "}) {
        for (std::size_t at = data.find(prefix); at != std::string_view::npos;
             at = data.find(prefix, at + 1)) {
            const auto start = at + prefix.size();
            if (start + kStampLen > data.size()) continue;
            if (!looks_like_stamp(data.substr(start, kStampLen))) continue;
            spans.emplace_back(start, start + kStampLen);
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

bool bmi_equivalent(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto da = read_all(a);
    auto db = read_all(b);
    if (!da || !db) return false;
    // The stamps are fixed width, so equivalent BMIs always have equal size. A
    // size difference is a real difference, never a maskable one.
    if (da->size() != db->size()) return false;

    const auto sa = stamp_spans(*da);
    const auto sb = stamp_spans(*db);
    // Disagreement about WHERE the stamps are is itself a structural
    // difference; fall back to strict equality rather than guessing.
    if (sa != sb) return *da == *db;
    if (sa.empty()) return *da == *db;

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

Verify parse_verify(std::string_view value) {
    return value == "size" ? Verify::Size : Verify::Content;
}

std::expected<StageOutcome, StageError> stage_file(const std::filesystem::path& src,
                                                   const std::filesystem::path& dst,
                                                   const StageOptions& opts) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return std::unexpected(StageError{
            std::format("staging source does not exist: {}", src.string())});
    }

    if (!dst.parent_path().empty()) {
        std::error_code dec;
        std::filesystem::create_directories(dst.parent_path(), dec);
        if (dec && !std::filesystem::is_directory(dst.parent_path())) {
            return std::unexpected(StageError{
                std::format("cannot create '{}': {}",
                            dst.parent_path().string(), dec.message())});
        }
    }

    // Already staged? Content by default — size is the caller-opt-in shortcut
    // (see module comment).
    std::error_code sec;
    if (std::filesystem::is_regular_file(dst, sec)) {
        std::error_code se1, se2;
        auto sizeDst = std::filesystem::file_size(dst, se1);
        auto sizeSrc = std::filesystem::file_size(src, se2);
        bool equivalent = !se1 && !se2 && sizeDst == sizeSrc
            && (opts.verify == Verify::Size || same_content(src, dst));
        if (equivalent) {
            // Timestamps left untouched on purpose — see module comment.
            return StageOutcome{.copied = false};
        }
    }

    auto delay = opts.backoff;
    std::error_code last;
    for (int attempt = 0; attempt <= opts.retries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(delay);
            delay *= 3;
        }
        last = write_once(src, dst);
        if (!last) return StageOutcome{.copied = true};
    }

    return std::unexpected(StageError{failure_message(src, dst, last)});
}

} // namespace mcpp::build::stage
