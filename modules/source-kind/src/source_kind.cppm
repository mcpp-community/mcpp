// mcpp.source_kind — what ROLE a source file plays in the build, decided once.
//
// WHY THIS MODULE EXISTS
//
// "Which extension means what" used to be derived in twenty places across
// nine files, in eight mutually inconsistent lists. Three different answers to
// "is this an implementation unit" (`.cpp` in the scanner, `.cpp`/`.cxx` in the
// p1689 reader, eight extensions in the planner); four different answers to
// "is this a source file at all" (the fast-path sweep, the build-program output
// check, the default glob, the staging fallback). Adding `.ixx` to one of them
// silently left the other seven unchanged — which is exactly how mcpp#272 fixed
// link-object collection while `pick_rule` still routed the same file to a rule
// that emits no BMI.
//
// The repair is not "everyone calls the same function". It is:
//
//     classification happens ONCE, where the file enters the graph, and is
//     carried as data from there on.
//
// `SourceUnit::kind` is set by the scanner (which already has the owning
// package's manifest in hand); `CompileUnit::kind` is copied from it by the
// planner; every consumer downstream reads the field and never looks at an
// extension again. The one legitimate re-derivation is the fast path, which
// runs BEFORE prepare and therefore has no plan — it calls classify() with the
// table from the same manifest the scanner will use, so the two cannot drift.
//
// WHY A LEAF MODULE (import std, nothing else)
//
// `mcpp.manifest.toml` needs it (default globs, auto-target inference) and
// `mcpp.modgraph.scanner` needs it, while every `mcpp.build.*` module imports
// `mcpp.manifest`. Living under src/build/ would make the dependency circular.
// Same argument as mcpp.version: a leaf can be used by anyone. Keep it a leaf.
//
// WHY EXTENSIONS ARE COMPARED LITERALLY (no case folding)
//
// `.S` and `.s` are DIFFERENT LANGUAGES here — `.S` goes through the C
// preprocessor, `.s` does not. Case is load-bearing in this domain, so
// normalization only trims and supplies a missing leading dot. This also
// preserves today's semantics exactly: every site being replaced compared
// extensions literally.

export module mcpp.source_kind;

import std;

export namespace mcpp {

// The role a file plays. Deliberately about the BUILD GRAPH, not about
// language dialects: two files with the same kind are handled the same way by
// the planner, the backend and the caches.
enum class SourceKind {
    // Provides a module interface: compiles with the module rule, emits a BMI,
    // and its object is linked unconditionally (module initializers can run
    // even when nothing references a symbol in it).
    ModuleInterface,
    // C++ translation unit that is not a module interface.
    Cxx,
    // C / Objective-C. Routed to the C compile rule; never scanned for imports
    // (a benign `import_foo` identifier must not be misparsed).
    C,
    // GNU assembler. `.S` is preprocessed, `.s` is not — the distinction lives
    // in the extension, not in the kind, because it selects a compile rule
    // within one role rather than a different role.
    GasAsm,
    // NASM syntax.
    NasmAsm,
    // Compiled by a vendor device compiler mcpp does not drive directly
    // (nvcc, hipcc). Never scanned for imports and never produces a BMI: no
    // such compiler accepts C++20 modules, so a device unit cannot be one.
    //
    // The kind states the GRAPH ROLE, not the language. That is deliberate:
    // it lets one kind cover CUDA C++, HIP, and device dialects that are not
    // C++ at all (Intel Gaudi's TPC-C is derived from C99), without the
    // classification table growing a row per vendor.
    Device,
    // Not compiled, but editing one can change what the graph should be.
    Header,
    // Not a build input.
    Other,
};

std::string_view to_string(SourceKind k);

// Which extensions name a module interface. Everything else about
// classification is fixed: the C / assembly / header axes have been complete
// and stable since they were introduced, so this is the only axis a project
// can extend.
//
// The struct (rather than a bare vector) is the room to grow: adding a
// `cxx` axis later is one member plus one parse site, and no signature in this
// file changes. Resist filling it in speculatively — an axis nobody asked for
// is an axis nobody has tested.
struct ExtensionTable {
    // Always contains the built-ins first, in their historical order.
    std::vector<std::string> moduleInterface;
};

// Trim, then supply a leading dot if absent. Does NOT change case — see the
// header comment. Returns an empty string for input that cannot name an
// extension (empty, or "." alone), which callers must reject.
std::string normalize_extension(std::string_view raw);

// The built-in table. `.cppm` ONLY — `.ccm` / `.cxxm` / `.ixx` are opt-in via
// `[build] module_extensions`.
//
// This is a compatibility decision, not a conservatism reflex. Widening the
// built-in set widens the DEFAULT source glob with it, so a published package
// with a vendored MSVC-only `.ixx` under src/ would start compiling it on the
// next mcpp upgrade — a break its author cannot fix, because the tarball for
// that version has already shipped.
ExtensionTable builtin_extension_table();

// Built-ins plus a package's own `[build] module_extensions`. Entries are
// normalized and de-duplicated; already-built-in and empty entries are
// dropped. Validation of hostile entries is `validate_module_extensions`
// below — this function never fails, so that a manifest which failed
// validation still classifies exactly like a default one.
ExtensionTable extension_table_for(std::span<const std::string> extras);

// Extensions that already name a non-module role. Declaring one of these as a
// module interface has no legitimate use and would route (say) a C file to the
// C++ module rule, failing somewhere that names neither the file nor the key.
// Rejected at parse time as a hard error.
bool is_reserved_non_module_extension(std::string_view normalizedExt);

// nullopt = fine. Otherwise the (already formatted) reason, for a manifest
// error. Checks each entry in order so the message names the first offender.
std::optional<std::string>
validate_module_extensions(std::span<const std::string> extras);

SourceKind classify(const std::filesystem::path& p, const ExtensionTable& t);

// ─── Predicates derived from the kind ────────────────────────────────────
//
// These exist so that a consumer interested in ONE axis does not write its own
// switch — which is how the eight inconsistent lists happened in the first
// place.

// Compiles with the module rule and emits a BMI.
bool produces_bmi(SourceKind k);
// Its object is linked even when nothing references it.
bool links_unconditionally(SourceKind k);
// Cannot contain `import` / `module`: no P1689 scan, no BMI implicit inputs.
bool is_scan_exempt(SourceKind k);
// A C++ translation unit or module interface — i.e. compiled by the C++
// driver rather than the C driver or an assembler.
bool is_cxx_like(SourceKind k);
// Editing a file of this kind can change what the graph SHOULD be, so the
// fast path must fall through to a full prepare.
//
// Assembly is deliberately absent: an assembly unit has no `import` and no
// scanned include graph, so editing one changes its object's content (which
// ninja tracks on its own) but never the shape of the graph. A NEW assembly
// file is a different question, and glob_inputs_stale answers it.
bool affects_graph_shape(SourceKind k);

// The convention default for `[build] sources`, derived from the table rather
// than written beside it. Two hand-maintained copies of this list already
// existed and had already drifted apart (the staging fallback was missing all
// three assembly extensions).
std::vector<std::string> default_source_globs(const ExtensionTable& t);

// The human-readable form of the above, for `inferredNotes`. Derived for the
// same reason.
std::string default_source_globs_note(const ExtensionTable& t);

// ─── Object file naming ──────────────────────────────────────────────────

// How a source's object file is named. Three cases, and the reason there are
// three rather than one is compatibility, not taste.
enum class ObjectNaming {
    // `foo.cpp` -> `foo.o`
    Stem,
    // `foo.cppm` -> `foo.m.o`
    StemDotM,
    // `foo.ixx` -> `foo.ixx.o`
    FullFilename,
};

// An object's name is part of the INTERNAL LAYOUT of a global cache entry
// (`CompileUnit::packageObjectRel`). Renaming one without changing the cache
// key does not produce a cache miss — it produces a HIT on an entry that does
// not contain the object the link step then asks for. So every extension that
// has a historical name keeps it, and only extensions that had no name before
// (anything a project adds via `module_extensions`) get the collision-proof
// full-filename form that assembly has always used.
//
// The function is total and monotone: adding a new extension can never change
// an existing name.
//
// KNOWN GAP, deliberately not closed here: `foo.c` and `foo.cpp` in the same
// directory both answer `Stem` and therefore collide, as they always have.
// Fixing it means renaming C objects, which is precisely the cache-layout
// change described above and needs a cache-key revision to be safe. Tracked
// separately; `tests/unit/test_source_kind.cpp` pins it as a known gap rather
// than weakening the exhaustive no-collision assertion around it.
ObjectNaming object_naming_for(const std::filesystem::path& src);

// The object file's NAME, formatted from the policy above.
//
// It lived in plan.cppm as an internal helper, under a comment saying the
// policy lives here and it "only formats it" — which is exactly the split
// worth closing: a second reader of the policy is a second place the format
// can drift. `mcpp pack` needs the same answer (to tell which archive members
// belong to the interface units it is publishing as source), and reaching into
// plan.cppm for it would drag the whole planner into the packer.
std::string object_filename_for(const std::filesystem::path& src,
                                std::string_view objExt = ".o");

} // namespace mcpp

namespace mcpp {

namespace {

// Historical object-naming sets. Literal, and intentionally NOT derived from
// SourceKind: the question here is "did this extension have a name before",
// which is a fact about mcpp's history, not about the language.
constexpr std::string_view kStemNamed[] = {
    ".cpp", ".cc", ".cxx", ".c", ".m", ".mm",
};
constexpr std::string_view kFullFilenameNamed[] = {
    ".S", ".s", ".asm",
};

constexpr std::string_view kCxxExtensions[]    = { ".cpp", ".cc", ".cxx", ".mm" };
constexpr std::string_view kCExtensions[]      = { ".c", ".m" };
constexpr std::string_view kGasExtensions[]    = { ".S", ".s" };
constexpr std::string_view kNasmExtensions[]   = { ".asm" };
constexpr std::string_view kHeaderExtensions[] = { ".h", ".hpp", ".hh", ".hxx" };
// Device sources and the headers they include. The headers are classified as
// `Header` rather than `Device` because their role is the header role: they
// are not compiled, and editing one can change what the graph should be.
constexpr std::string_view kDeviceExtensions[]       = { ".cu", ".hip" };
constexpr std::string_view kDeviceHeaderExtensions[] = { ".cuh", ".hiph" };

bool contains(std::span<const std::string_view> set, std::string_view ext) {
    for (auto e : set) if (e == ext) return true;
    return false;
}

} // namespace

std::string_view to_string(SourceKind k) {
    switch (k) {
        case SourceKind::ModuleInterface: return "module-interface";
        case SourceKind::Cxx:             return "c++";
        case SourceKind::C:               return "c";
        case SourceKind::GasAsm:          return "gas";
        case SourceKind::NasmAsm:         return "nasm";
        case SourceKind::Device:          return "device";
        case SourceKind::Header:          return "header";
        case SourceKind::Other:           return "other";
    }
    return "other";
}

std::string normalize_extension(std::string_view raw) {
    std::size_t b = 0, e = raw.size();
    while (b < e && (raw[b] == ' ' || raw[b] == '\t')) ++b;
    while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) --e;
    auto trimmed = raw.substr(b, e - b);
    if (trimmed.empty() || trimmed == ".") return {};
    if (trimmed.front() == '.') return std::string(trimmed);
    return "." + std::string(trimmed);
}

ExtensionTable builtin_extension_table() {
    // `.cppm` alone, on purpose. `.ixx`, `.ccm`, `.cxxm` and anything else are
    // the PROJECT's to declare via `[build] module_extensions` — the extension
    // set is configuration, not a built-in list that mcpp grows one entry at a
    // time as extensions come into fashion.
    //
    // What has to be true for that to be a real knob is that a DECLARED
    // extension works everywhere without further help, and it does: a module
    // interface's language is stated explicitly per toolchain
    // (`BmiTraits::moduleInterfaceLangFlag` — `/interface /TP`,
    // `-x c++-module`, `-x c++`) rather than inferred by the driver from the
    // extension. Clang's driver not recognising `.ixx` is therefore beside the
    // point: mcpp never lets it guess.
    //
    // What was NOT true, and is fixed in the scanner rather than here: an
    // UNdeclared extension used to be accepted silently. See the classification
    // check there for what that cost.
    return ExtensionTable{ .moduleInterface = { ".cppm" } };
}

ExtensionTable extension_table_for(std::span<const std::string> extras) {
    auto t = builtin_extension_table();
    for (auto const& raw : extras) {
        auto ext = normalize_extension(raw);
        if (ext.empty()) continue;
        if (std::ranges::find(t.moduleInterface, ext) != t.moduleInterface.end())
            continue;
        t.moduleInterface.push_back(std::move(ext));
    }
    return t;
}

bool is_reserved_non_module_extension(std::string_view ext) {
    return contains(kCxxExtensions, ext)    || contains(kCExtensions, ext)
        || contains(kGasExtensions, ext)    || contains(kNasmExtensions, ext)
        || contains(kHeaderExtensions, ext) || contains(kDeviceExtensions, ext)
        || contains(kDeviceHeaderExtensions, ext);
}

std::optional<std::string>
validate_module_extensions(std::span<const std::string> extras) {
    for (auto const& raw : extras) {
        auto ext = normalize_extension(raw);
        if (ext.empty()) {
            return std::format(
                "[build].module_extensions contains an empty entry ('{}'); "
                "each entry names a file extension, e.g. \".ixx\"", raw);
        }
        if (ext.find('/') != std::string::npos
            || ext.find('\\') != std::string::npos
            || ext.find('*') != std::string::npos) {
            return std::format(
                "[build].module_extensions entry '{}' is not an extension. "
                "This key takes extensions (\".ixx\"), not paths or globs — "
                "use [build].sources to choose WHICH files are built.", raw);
        }
        if (ext.find('.', 1) != std::string::npos) {
            return std::format(
                "[build].module_extensions entry '{}' has more than one dot; "
                "an extension is the final segment only (\".ixx\")", raw);
        }
        if (is_reserved_non_module_extension(ext)) {
            return std::format(
                "[build].module_extensions cannot claim '{}': it already names "
                "a non-module source role, and declaring it as a module "
                "interface would route those files to the C++ module rule.\n"
                "       If you have module interfaces with an unusual "
                "extension, name that extension instead.", ext);
        }
    }
    return std::nullopt;
}

SourceKind classify(const std::filesystem::path& p, const ExtensionTable& t) {
    auto ext = p.extension().string();
    if (ext.empty()) return SourceKind::Other;
    // The project's axis wins: a package that declares `.ixx` means it.
    // It can never shadow a non-module role — validate_module_extensions
    // rejects those entries before they reach a table.
    for (auto const& m : t.moduleInterface)
        if (ext == m) return SourceKind::ModuleInterface;
    if (contains(kCxxExtensions, ext))    return SourceKind::Cxx;
    if (contains(kCExtensions, ext))      return SourceKind::C;
    if (contains(kGasExtensions, ext))    return SourceKind::GasAsm;
    if (contains(kNasmExtensions, ext))   return SourceKind::NasmAsm;
    if (contains(kDeviceExtensions, ext)) return SourceKind::Device;
    if (contains(kHeaderExtensions, ext)
        || contains(kDeviceHeaderExtensions, ext)) return SourceKind::Header;
    return SourceKind::Other;
}

bool produces_bmi(SourceKind k) { return k == SourceKind::ModuleInterface; }

bool links_unconditionally(SourceKind k) { return k == SourceKind::ModuleInterface; }

bool is_scan_exempt(SourceKind k) {
    return k == SourceKind::C      || k == SourceKind::GasAsm
        || k == SourceKind::NasmAsm || k == SourceKind::Device;
}

bool is_cxx_like(SourceKind k) {
    return k == SourceKind::ModuleInterface || k == SourceKind::Cxx;
}

bool affects_graph_shape(SourceKind k) {
    return k == SourceKind::ModuleInterface || k == SourceKind::Cxx
        || k == SourceKind::C              || k == SourceKind::Header;
}

std::vector<std::string> default_source_globs(const ExtensionTable& t) {
    std::vector<std::string> globs;
    globs.reserve(t.moduleInterface.size() + 6);
    for (auto const& e : t.moduleInterface) globs.push_back("src/**/*" + e);
    // Assembly in the tree almost certainly wants building; a project that
    // vendors foreign-syntax `.asm` can `!`-exclude it. (`.cxx` has never been
    // in the convention default and is not added here — that would be a
    // behavioral change wearing a refactor's clothes.)
    globs.push_back("src/**/*.cpp");
    globs.push_back("src/**/*.cc");
    globs.push_back("src/**/*.c");
    globs.push_back("src/**/*.S");
    globs.push_back("src/**/*.s");
    globs.push_back("src/**/*.asm");
    // Device extensions are deliberately absent. The argument is the one that
    // keeps the built-in module-extension table at `.cppm` alone: widening the
    // default glob makes a published package that vendors a `.cu` it builds
    // elsewhere start compiling it on the next mcpp upgrade, and that is a
    // break its author cannot fix because the tarball has already shipped.
    // Device sources are opted into by naming them in a device target.
    return globs;
}

std::string default_source_globs_note(const ExtensionTable& t) {
    std::string s = "sources [src/**/*.{";
    bool first = true;
    for (auto const& e : t.moduleInterface) {
        if (!first) s += ',';
        first = false;
        s += e.substr(1);            // drop the leading dot
    }
    s += ",cpp,cc,c,S,s,asm}]";
    return s;
}

ObjectNaming object_naming_for(const std::filesystem::path& src) {
    auto ext = src.extension().string();
    if (contains(kFullFilenameNamed, ext)) return ObjectNaming::FullFilename;
    if (ext == ".cppm")                    return ObjectNaming::StemDotM;
    if (contains(kStemNamed, ext))         return ObjectNaming::Stem;
    // Anything with no historical name — every extension a project can add
    // via `module_extensions`, and any stray input — gets the collision-proof
    // form. `foo.ixx` -> `foo.ixx.o` can collide with neither `foo.cppm` ->
    // `foo.m.o` nor `foo.cpp` -> `foo.o`.
    return ObjectNaming::FullFilename;
}

std::string object_filename_for(const std::filesystem::path& src,
                                std::string_view objExt) {
    switch (object_naming_for(src)) {
        case ObjectNaming::StemDotM:
            return src.stem().string() + ".m" + std::string(objExt);
        case ObjectNaming::Stem:
            return src.stem().string() + std::string(objExt);
        case ObjectNaming::FullFilename:
            break;
    }
    // Assembly siblings of a C/C++ TU commonly share its stem (foo.c +
    // foo.asm); keeping the full extension means they can never collide —
    // the per-package collision prefix can't help two same-stem files in the
    // same directory. Every extension a project adds via
    // `[build] module_extensions` lands here for the same reason.
    return src.filename().string() + std::string(objExt);
}

} // namespace mcpp
