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
//      identical to the source needs no write, whatever the reason. A
//      destination that cannot be read is not compared: a holder may deny
//      opening it (Windows: a MemoryMappedFile opened with FileShare.None),
//      and then it cannot be replaced either. `--verify size` accepts an equal
//      size for such a destination, and only for it. A destination that can be
//      read is compared under every mode: an equal size does not make a staged
//      file the source's. A package's BMI lands at a path named by the module,
//      not by the cache entry, so the BMI of the version before an upgrade
//      sits where the new one goes, and GCC writes an interface whose source
//      did not change at the same size with a different checksum; importers
//      then fail with "CRC mismatch". The size-only shortcut is never the
//      default, because staging also carries .dll payloads whose PE section
//      padding makes "same size, different bytes" ordinary.
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
    Size,       // also an equal size where the destination cannot be read
                // (MCPP_STAGE_VERIFY=size / --verify size)
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

// Publish one destination that may have more than one source (SPEC-007 R4.2,
// mcpp#723): two or more packages of one graph can each generate a
// byte-identical file and deploy it under the same name, and the plan no
// longer refuses that at planning time — a generated source may not exist yet
// when the plan is built, so its content cannot be compared there. This is
// where the invariant is actually checked, because by build time every source
// exists: every source is compared against the first by content
// (`same_content`, never masking the way `mcpp::bmi::gcc::equivalent` does — a deploy
// target is an ordinary file, not a BMI with a compiler-embedded clock), and
// the first is staged when they all agree. Disagreement is refused, naming
// every source and the destination, so the message points at every producer
// instead of an arbitrary pair.
//
// `srcs.size() == 1` is not a special case bolted on top: it takes the same
// path as every other count, with the comparison loop simply empty, and ends
// up calling `stage_file` on that one source exactly as before this function
// existed. This is what keeps a project with one source per destination —
// every project before this feature — emitting the same `mcpp stage`
// invocation it always has.
std::expected<StageOutcome, StageError> stage_files(
    const std::vector<std::filesystem::path>& srcs,
    const std::filesystem::path& dst,
    const StageOptions& opts = {});

// Byte-for-byte comparison (exported for tests). False when either file is
// unreadable or the sizes differ.
bool same_content(const std::filesystem::path& a, const std::filesystem::path& b);

// Parse a --verify / MCPP_STAGE_VERIFY value. Unknown values fall back to the
// safe default (Content).
Verify parse_verify(std::string_view value);

} // namespace mcpp::build::stage

namespace mcpp::build::stage {

namespace {

std::filesystem::path temp_sibling(const std::filesystem::path& dst) {
    // Same directory as the destination so the rename stays on one filesystem.
    // The writer's own name: processes that publish one destination at once
    // (a host tool built by two builds of one home) must not share a temporary,
    // and a clock alone is a microsecond on macOS.
    auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    auto name = dst.filename().string()
              + std::format(".tmp.{}.{:x}", stamp, std::random_device{}());
    return dst.parent_path() / name;
}

// A shared library is placed by a hard link where the file system allows one
// (workspace design 2026-09-29 §5.3): several product directories that load
// the same DLL or shared object then hold one copy of its bytes. Only shared
// libraries are linked. A program may write a data file that lies beside it,
// and through a link that write would reach the file's source, a package's own
// tree or a payload store; a loader never writes the libraries it loads.
bool links_on_placement(const std::filesystem::path& p) {
    const auto name = p.filename().generic_string();
    auto lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.ends_with(".dll") || lower.ends_with(".dylib")
        || lower.ends_with(".so") || lower.find(".so.") != std::string::npos;
}

// One full write attempt: temp+rename first, in-place overwrite as fallback.
// The temporary is a hard link to the source when `links_on_placement` admits
// it and the link can be made, and a copy otherwise (another volume, FAT or
// exFAT, a permission), so the fallback is today's behaviour.
// Returns an empty error_code on success; otherwise the most informative error
// (the in-place one — that's where 1224 / 32 shows up), or the rename's when
// the destination shares its bytes with another name and is not written in
// place.
std::error_code write_once(const std::filesystem::path& src,
                           const std::filesystem::path& dst) {
    auto tmp = temp_sibling(dst);
    std::error_code ec;
    bool linked = false;
    if (links_on_placement(src)) {
        std::error_code lec;
        std::filesystem::create_hard_link(src, tmp, lec);
        linked = !lec;
    }
    if (!linked)
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
        ec = rec;
    } else {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
    }

    // A destination placed earlier by a link shares its bytes with that
    // placement's source, so it is never written in place.
    std::error_code nec;
    if (const auto links = std::filesystem::hard_link_count(dst, nec); !nec && links > 1)
        return ec;

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
        "help: another process has this file memory-mapped, loaded or open.\n"
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

    // Already staged? Equal content; under `Verify::Size` also an equal size
    // when the destination cannot be read (see module comment).
    std::error_code sec;
    if (std::filesystem::is_regular_file(dst, sec)) {
        std::error_code se1, se2;
        auto sizeDst = std::filesystem::file_size(dst, se1);
        auto sizeSrc = std::filesystem::file_size(src, se2);
        bool equivalent = !se1 && !se2 && sizeDst == sizeSrc
            && (same_content(src, dst)
                || (opts.verify == Verify::Size && !std::ifstream(dst, std::ios::binary).is_open()));
        if (equivalent) {
            // A shared library staged as a copy -- by a release before
            // placements were linked, or where a link once failed -- is placed
            // again by a link, so a library every member's directory loads is
            // one file and not one per member (mcpp#874: 650 MB of copies
            // across 27 product directories). A link that cannot be made
            // leaves the equal copy where it is.
            std::error_code qec;
            if (links_on_placement(src) && !std::filesystem::equivalent(src, dst, qec) && !qec) {
                auto tmp = temp_sibling(dst);
                std::error_code lec, rec;
                std::filesystem::create_hard_link(src, tmp, lec);
                if (!lec) {
                    std::filesystem::rename(tmp, dst, rec);
                    if (rec) std::filesystem::remove(tmp, rec);
                }
            }
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

std::expected<StageOutcome, StageError> stage_files(
    const std::vector<std::filesystem::path>& srcs,
    const std::filesystem::path& dst,
    const StageOptions& opts)
{
    if (srcs.empty()) {
        return std::unexpected(StageError{
            std::format("staging '{}' requires at least one source", dst.string())});
    }
    // A source that is not there is named as such: the step that should have
    // written it failed or does not exist, which is not a disagreement.
    if (srcs.size() > 1)
        for (auto const& s : srcs) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(s, ec))
                return std::unexpected(StageError{std::format(
                    "a source of '{}' does not exist: {}", dst.string(), s.string())});
        }
    for (std::size_t i = 1; i < srcs.size(); ++i) {
        if (!same_content(srcs.front(), srcs[i])) {
            std::string list;
            for (auto const& s : srcs) list += std::format("\n  {}", s.string());
            return std::unexpected(StageError{std::format(
                "two or more sources disagree for one destination\n"
                "  destination: {}\n"
                "  sources:{}",
                dst.string(), list)});
        }
    }
    return stage_file(srcs.front(), dst, opts);
}

} // namespace mcpp::build::stage
