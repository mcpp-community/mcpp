// mcpp.pack.strip — removing debug information from a SHIPPED artifact.
//
// WHY A PACKAGING STEP AND NOT `[profile.<n>].strip`
//
// mcpp already has a `strip` axis: `[profile.dist].strip` appends `-s` to the
// LINK. It cannot do this job, and the reason is not a detail:
//
//   * a `-s` link never touches a static archive, and a static archive is what
//     most library packages ship;
//   * `-s` is strip-ALL, which is the one thing an archive must not have done
//     to it (below);
//   * a link-time strip discards the symbols instead of separating them, so
//     there is no `.debug` file to keep.
//
// So the two are different decisions with different inputs, and they are named
// differently: `[profile].strip` is "how is this built", `[pack].strip` is
// "what travels". Documented in docs/02 and docs/12.
//
// ⚠️ THE DIVISION BY ARTIFACT SHAPE IS LOAD-BEARING, AND IT IS MEASURED
//
// `strip --strip-all` on a `.a` removes the ARCHIVE SYMBOL INDEX, and the
// package then fails at the consumer's link with a message that names neither
// strip nor the publisher:
//
//   ld: ./libdep.a: error adding symbols: archive has no index; run ranlib to add one
//
// while `--strip-debug` on the same archive links and runs (measured: 2988 →
// 1244 bytes, `ok=42`). A shared library is the opposite case — it may lose
// its `.symtab` but must keep `.dynsym`, which is what `--strip-unneeded`
// means. So there is a table, and it is dh_strip's:
//
//   executable       --strip-all        (nothing links against it)
//   shared library   --strip-unneeded   (keeps .dynsym = the exports)
//   static archive   --strip-debug      (keeps .symtab = the archive index)
//                    + --enable-deterministic-archives
//
// `--remove-section=.comment --remove-section=.note` goes on all three, also
// from dh_strip. It does NOT match `.note.gnu.build-id`: section removal is by
// exact name, so the build-id survives and `--add-gnu-debuglink` still has
// something to pair with.
//
// EMPTY TOOL IS NOT ALWAYS AN ERROR, AND WHICH CASE IT IS BELONGS TO THE
// TARGET FORMAT — NOT TO THE COMPILER
//
// Two of the three formats keep debug information OUTSIDE the image, and for
// them an absent `strip` is the right answer rather than a missing dependency:
//
//   PE/MSVC    a separate `.pdb`, by design.
//   Mach-O     the linked image carries a DEBUG MAP (N_OSO stanzas naming the
//              `.o` files); the DWARF itself never enters the `.dylib` unless
//              `dsymutil` is run, and what it then produces is a `.dSYM`
//              bundle beside the image. `strip` there would remove the symbol
//              table — a different thing — and `objcopy --only-keep-debug` has
//              nothing to copy.
//
// ELF and PE/MinGW carry DWARF inside the image, so a missing `strip` there is
// a REFUSAL — the same stance `run_library_pack` already takes for a missing
// archiver, and for the same reason: shipping the artifact anyway is the
// silent-wrong-answer this feature exists to remove.
//
// ⚠️ KEYING THIS ON THE COMPILER WOULD BE WRONG IN BOTH DIRECTIONS. clang
// targeting `x86_64-windows-msvc` produces `.pdb` debug info and would be
// asked to strip in-band; Apple's clang ships no `llvm-strip`, so a
// compiler-keyed rule would REFUSE every `mcpp pack` on macOS for a format that
// has nothing to strip in the first place.

export module mcpp.pack.strip;

import std;
import mcpp.pack.binfmt;
import mcpp.platform;

export namespace mcpp::pack {

// What the artifact IS. Taken from the link unit's kind, never guessed from
// the extension: `libfoo.a` and `foo.lib` are the same shape under different
// names, and a `.so` that is really a linker script is neither.
enum class ArtifactShape { Executable, SharedLibrary, StaticArchive };

// The tools for ONE leg, resolved from that leg's toolchain by the caller
// (mcpp.toolchain.binutils_tool). Both may be empty; see the header for when
// that is an answer and when it is a refusal.
struct StripTools {
    std::filesystem::path strip;
    std::filesystem::path objcopy;
    // Does this leg's format carry debug information inside the image?
    // Ask `debug_info_is_in_band` rather than filling this by hand — see the
    // header for the two formats where the answer is no, and for why the
    // question is about the TARGET and not about the compiler.
    bool                  inBandDebugInfo = true;
};

// Is debug information carried INSIDE an image built for `canonicalTriple`?
//
// A string question about the canonical triple (`arch-os[-env]`), deliberately:
// this module knows nothing about toolchains, and both packers have already
// resolved that triple by the time they ask.
bool debug_info_is_in_band(std::string_view canonicalTriple);

enum class StripOutcome { Stripped, NotApplicable };

struct StripResult {
    StripOutcome          outcome = StripOutcome::NotApplicable;
    std::filesystem::path debugFile;      // written only when a debug dir was given
    std::uintmax_t        before = 0;
    std::uintmax_t        after  = 0;
};

// The strip arguments for `shape`. Exported for its unit test — the table
// above is the whole feature, and a test that has to spawn a real `strip` to
// read it back would run on one platform out of three.
std::vector<std::string> strip_args(ArtifactShape shape);

// Separate the debug information (when `debugDir` is non-empty), then strip.
//
// ORDER IS NOT FREE: `--only-keep-debug` has to read the artifact while it
// still has its debug sections, and `--add-gnu-debuglink` has to write into
// the artifact after they are gone. Doing it the other way round produces a
// `.debug` file with no debug information in it and no diagnostic.
std::expected<StripResult, std::string>
strip_artifact(const std::filesystem::path& artifact,
               ArtifactShape shape,
               const StripTools& tools,
               const std::filesystem::path& debugDir);

} // namespace mcpp::pack

namespace mcpp::pack {

namespace {

std::expected<void, std::string> run_tool(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd + " 2>&1");
    if (r.exit_code != 0)
        return std::unexpected(std::format(
            "  command: {}\n  output : {}", cmd, r.output));
    return {};
}

std::string join_quoted(const std::filesystem::path& tool,
                        const std::vector<std::string>& args,
                        const std::filesystem::path& file)
{
    std::string cmd = mcpp::platform::shell::quote(tool.string());
    for (auto const& a : args) cmd += " " + mcpp::platform::shell::quote(a);
    cmd += " " + mcpp::platform::shell::quote(file.string());
    return cmd;
}

std::uintmax_t size_of(const std::filesystem::path& p) {
    std::error_code ec;
    auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : n;
}

} // namespace

bool debug_info_is_in_band(std::string_view canonicalTriple) {
    // `arch-os[-env]`. Splitting rather than substring-matching: an arch or a
    // vendor segment could contain either of these words, and mcpp has been
    // bitten before by a triple predicate that answered on a substring.
    std::vector<std::string_view> seg;
    for (std::size_t i = 0; i <= canonicalTriple.size(); ) {
        auto j = canonicalTriple.find('-', i);
        if (j == std::string_view::npos) { seg.push_back(canonicalTriple.substr(i)); break; }
        seg.push_back(canonicalTriple.substr(i, j - i));
        i = j + 1;
    }
    if (seg.size() >= 2 && seg[1] == "macos") return false;   // debug map + .dSYM
    if (seg.size() >= 3 && seg[2] == "msvc")  return false;   // separate .pdb
    return true;
}

std::vector<std::string> strip_args(ArtifactShape shape) {
    // dh_strip's own division. See the header for the measurement behind the
    // archive row — it is the one that turns a package into an unlinkable one.
    std::vector<std::string> args{ "--remove-section=.comment",
                                   "--remove-section=.note" };
    switch (shape) {
        case ArtifactShape::Executable:
            args.push_back("--strip-all");
            break;
        case ArtifactShape::SharedLibrary:
            args.push_back("--strip-unneeded");
            break;
        case ArtifactShape::StaticArchive:
            args.push_back("--strip-debug");
            // Zero the member uid/gid/timestamps while we are rewriting the
            // archive anyway: two identical packs should produce identical
            // bytes, and this is the one place the packer touches them.
            args.push_back("--enable-deterministic-archives");
            break;
    }
    return args;
}

std::expected<StripResult, std::string>
strip_artifact(const std::filesystem::path& artifact,
               ArtifactShape shape,
               const StripTools& tools,
               const std::filesystem::path& debugDir)
{
    StripResult out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(artifact, ec))
        return std::unexpected(std::format("'{}' is not a file", artifact.string()));

    if (!tools.inBandDebugInfo) {
        // PE/MSVC: the `.pdb` is a separate file and was never inside this one.
        out.outcome = StripOutcome::NotApplicable;
        return out;
    }
    if (tools.strip.empty())
        return std::unexpected(std::format(
            "no `strip` was resolved for this target, so '{}' would ship with its "
            "debug information and the publisher's absolute source paths.\n"
            "  Pass `--no-strip` to ship it as built, or install the binutils that "
            "match this toolchain.",
            artifact.filename().string()));

    out.before = size_of(artifact);

    // ── separate, when asked ──────────────────────────────────────────
    //
    // Not for archives: a `.a` is a container of relocatable objects and
    // `--only-keep-debug` on one produces something no debugger consumes.
    // dh_strip makes the same exclusion.
    const bool separate = !debugDir.empty() && shape != ArtifactShape::StaticArchive;
    if (separate) {
        if (tools.objcopy.empty())
            return std::unexpected(std::format(
                "`--debug-symbols` was given but no `objcopy` was resolved for this "
                "target, so the debug information for '{}' cannot be separated.",
                artifact.filename().string()));
        std::filesystem::create_directories(debugDir, ec);
        if (ec) return std::unexpected(std::format(
            "cannot create '{}': {}", debugDir.string(), ec.message()));
        out.debugFile = debugDir / (artifact.filename().string() + ".debug");
        auto cmd = std::format("{} --only-keep-debug {} {}",
            mcpp::platform::shell::quote(tools.objcopy.string()),
            mcpp::platform::shell::quote(artifact.string()),
            mcpp::platform::shell::quote(out.debugFile.string()));
        if (auto r = run_tool(cmd); !r) return std::unexpected(std::format(
            "cannot separate debug information from '{}'.\n{}",
            artifact.string(), r.error()));
    }

    // ── strip ─────────────────────────────────────────────────────────
    if (auto r = run_tool(join_quoted(tools.strip, strip_args(shape), artifact)); !r)
        return std::unexpected(std::format(
            "cannot strip '{}'.\n{}", artifact.string(), r.error()));

    // ── and point the stripped artifact back at its symbols ───────────
    if (separate) {
        auto cmd = std::format("{} --add-gnu-debuglink={} {}",
            mcpp::platform::shell::quote(tools.objcopy.string()),
            mcpp::platform::shell::quote(out.debugFile.string()),
            mcpp::platform::shell::quote(artifact.string()));
        if (auto r = run_tool(cmd); !r) return std::unexpected(std::format(
            "cannot add the debug link to '{}'.\n{}", artifact.string(), r.error()));
    }

    out.after   = size_of(artifact);
    out.outcome = StripOutcome::Stripped;
    return out;
}

} // namespace mcpp::pack
