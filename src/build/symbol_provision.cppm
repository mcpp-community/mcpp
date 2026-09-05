// mcpp.build.symbol_provision — is every symbol in this image provided ONCE?
//
// THE INVARIANT
//
//   One library, one provider, one form.
//
// mcpp enforces it at two altitudes, because it has two kinds of knowledge.
// `mcpp.build.linkage_form` enforces it over what mcpp DECIDED — packages,
// targets, forms — at plan time, on every platform. This module enforces it
// over what the linker actually PRODUCED, and that is the only altitude that
// can see a library mcpp never knew about: one vendored inside a prebuilt
// package, one arriving through pkg-config, one belonging to the host.
//
// WHY AN IMAGE CAN HIJACK A LIBRARY IT DID NOT MEAN TO
//
// ELF has one flat symbol namespace and the executable is searched first. When
// an executable statically contains `inflate` and a shared object it links
// declares an undefined `inflate`, the linker adds the executable's definition
// to `.dynsym` so that reference will bind — no `-rdynamic` required, and mcpp
// passes none. At run time the shared object then calls the STATIC copy, and
// whatever `libz.so.1` sits beside it is dead weight. The link is silent, the
// loader is silent, and the program usually works.
//
// The mechanism is not a defect; it is what makes `malloc` interposition work.
// What was missing is anyone asking whether it happened on purpose.
//
// THE PREDICATE, IN TWO STAGES
//
//   EXPORTED = defined entries of .dynsym, minus copy relocations
//   CONFLICT = EXPORTED ∩ ⋃ defines(closure)
//
// Stage one is cheap and runs on every changed image; it is empty for a normal
// mcpp binary (measured: 0 of 217 dynamic symbols). Stage two runs only when
// stage one is not, and it is the stage that makes the report TRUE.
//
// STAGE TWO IS NOT OPTIONAL, and the reason is mcpp's own doing. A
// `kind = "shared"` dependency's link unit receives only ITS OWN objects
// (mcpp.build.plan), so a static package underneath it lands in the CONSUMER's
// executable instead, and the shared library binds back to it at run time.
// That is the shape above — arranged by mcpp, with exactly one copy of the
// code in the process, and completely benign. Reporting stage one alone would
// warn about a correct build that the user cannot do anything about, which is
// precisely the noise `mcpp.build.distribution` refuses to emit.
//
// Design: .agents/docs/2026-08-28-issue519-dependency-linkage-form.md §2.

export module mcpp.build.symbol_provision;

import std;
import mcpp.runtime.elf;

export namespace mcpp::build::symbol_provision {

// A definition this image contributes to the process-wide namespace.
struct Export {
    std::string name;
    bool        isFunc = false;
};

// One object that could also supply a symbol, as the report will name it.
struct Provider {
    std::string              label;    // a path, or "<pkg> (static, merged)"
    std::vector<std::string> defines;  // symbol names it defines
};

// A symbol with more than one provider in one image.
struct Conflict {
    std::string              name;
    bool                     isFunc = false;
    std::vector<std::string> alsoProvidedBy;
};

// FOUR-VALUED, and the last two are why.
//
//   Clean          the predicate applies, was evaluated, and held
//   Conflict       a symbol has two providers and the static one wins
//   NotApplicable  the predicate does not apply here (a static link, a shared
//                  library, an image whose author asked for exports)
//   NotEvaluated   it applies but could not be computed
//
// `NotApplicable` and `NotEvaluated` exist separately from `Clean` for the
// reason this repository keeps rediscovering: a check that reports "no
// findings" when it never ran is a check that goes green forever.
enum class Status { Clean, Conflict, NotApplicable, NotEvaluated };

std::string_view to_string(Status status);

struct Report {
    Status                status = Status::NotApplicable;
    // |EXPORTED| and the size of the whole dynamic symbol table. Both are
    // reported, always: "0" means nothing without the denominator beside it.
    std::size_t           exported = 0;
    std::size_t           total = 0;
    std::vector<Conflict> conflicts;
    // Why, for the two non-answers. Empty for Clean and Conflict.
    std::string           reason;

    bool actionable() const { return status == Status::Conflict; }
    // Human-readable body. Empty unless there is something to say.
    std::string explain(std::string_view artifact) const;
};

// Did this link ask for its symbols to be exported? Then the whole predicate
// is void: the author requested exactly the thing it detects.
//
// A FUNCTION over the flags rather than a boolean threaded from far away, so
// the vocabulary is written down once and can be tested without a linker.
bool export_dynamic_requested(std::span<const std::string> flags);

// Stage one. Every defined FUNC counts; a defined OBJECT counts unless a copy
// relocation sits at its address (libc data moved into this image).
//
// Returns nullopt when `symbols.copyRelocationsKnown` is false: without the
// machine's COPY relocation type every data symbol would look like a
// conflict, and answering wrongly is worse than declining.
std::optional<std::vector<Export>>
exported_definitions(const mcpp::platform::elf::DynamicSymbols& symbols);

// Stage two. `closure` is every object the image's loader will consult.
std::vector<Conflict> conflicting_exports(std::span<const Export> exports,
                                          std::span<const Provider> closure);

// The report an image with no dynamic symbol table gets, and the one an image
// whose author asked for exports gets. Named constructors rather than raw
// struct literals so every non-answer carries its reason.
Report not_applicable(std::string reason);
Report not_evaluated(std::string reason);

} // namespace mcpp::build::symbol_provision

namespace mcpp::build::symbol_provision {

std::string_view to_string(Status status) {
    switch (status) {
        case Status::Clean:         return "clean";
        case Status::Conflict:      return "conflict";
        case Status::NotApplicable: return "not-applicable";
        case Status::NotEvaluated:  return "not-evaluated";
    }
    return "not-evaluated";
}

bool export_dynamic_requested(std::span<const std::string> flags) {
    // Spellings, not a substring sweep: `--export-dynamic-symbol=foo` and
    // `--dynamic-list=x.txt` restrict the export set rather than requesting
    // one wholesale, but either way the author has taken over the decision.
    static constexpr std::string_view kExact[] = {
        "-rdynamic", "-export-dynamic", "--export-dynamic",
        "-Wl,--export-dynamic", "-Wl,-export-dynamic", "-Wl,-E",
    };
    static constexpr std::string_view kPrefixes[] = {
        "-Wl,--dynamic-list", "--dynamic-list",
        "-Wl,--export-dynamic-symbol", "--export-dynamic-symbol",
    };
    for (auto const& flag : flags) {
        for (auto exact : kExact) if (flag == exact) return true;
        for (auto prefix : kPrefixes) if (flag.starts_with(prefix)) return true;
    }
    return false;
}

std::optional<std::vector<Export>>
exported_definitions(const mcpp::platform::elf::DynamicSymbols& symbols) {
    if (!symbols.copyRelocationsKnown) return std::nullopt;
    std::vector<Export> out;
    for (auto const& symbol : symbols.defined) {
        // A copy relocation is never a function, so the address lookup is
        // only asked about data — which also keeps a FUNC that happens to
        // share an address with relocated data from being excused.
        if (!symbol.isFunc && symbols.copyRelocations.contains(symbol.value))
            continue;
        out.push_back(Export{ .name = symbol.name, .isFunc = symbol.isFunc });
    }
    std::ranges::sort(out, {}, &Export::name);
    return out;
}

std::vector<Conflict> conflicting_exports(std::span<const Export> exports,
                                          std::span<const Provider> closure) {
    std::vector<Conflict> out;
    for (auto const& exported : exports) {
        Conflict conflict{ .name = exported.name, .isFunc = exported.isFunc };
        for (auto const& provider : closure) {
            if (std::ranges::find(provider.defines, exported.name)
                != provider.defines.end())
                conflict.alsoProvidedBy.push_back(provider.label);
        }
        if (!conflict.alsoProvidedBy.empty()) out.push_back(std::move(conflict));
    }
    return out;
}

Report not_applicable(std::string reason) {
    return Report{ .status = Status::NotApplicable, .reason = std::move(reason) };
}

Report not_evaluated(std::string reason) {
    return Report{ .status = Status::NotEvaluated, .reason = std::move(reason) };
}

std::string Report::explain(std::string_view artifact) const {
    if (status != Status::Conflict || conflicts.empty()) return {};

    // Cap the list. The names are evidence, not an inventory — a zlib pulled
    // in twice contributes 86 of them, and a diagnostic nobody finishes
    // reading is a diagnostic nobody acts on.
    constexpr std::size_t kShown = 6;
    std::string body = std::format(
        "{}: {} symbol{} in this image {} also provided by a library it loads.\n",
        artifact, conflicts.size(), conflicts.size() == 1 ? "" : "s",
        conflicts.size() == 1 ? "is" : "are");

    std::set<std::string> providers;
    for (std::size_t i = 0; i < conflicts.size(); ++i) {
        auto const& conflict = conflicts[i];
        for (auto const& label : conflict.alsoProvidedBy) providers.insert(label);
        if (i < kShown)
            body += std::format("    {}{}\n", conflict.name,
                                conflict.isFunc ? "()" : "");
        else if (i == kShown)
            body += std::format("    ... and {} more\n", conflicts.size() - kShown);
    }

    body += "  Also provided by:\n";
    for (auto const& label : providers) body += std::format("    {}\n", label);

    // WHY it matters, then what to do — IN THE ORDER THAT ACTUALLY WORKS.
    //
    // `dependency_linkage = "shared"` is deliberately NOT first, and that
    // ordering was corrected against a measurement rather than reasoned. On a
    // real graph (a package staging glib, whose libgio pulls libz.so.1, plus a
    // statically built compat.zlib) switching the form does remove the
    // executable's 88 exported symbols — and then TWO zlibs load, because the
    // library mcpp builds is `libzlib.so` while the reference is to
    // `libz.so.1`. That is a worse state than the one being reported: two
    // copies instead of one, and no diagnostic at all, since the check
    // described above only looks at executables.
    //
    // So the first suggestion is the one that is always correct, and the form
    // switch is offered with the condition that makes it work.
    body +=
        "  The executable is searched first, so the copy inside it wins for\n"
        "  every symbol both provide — the library's own copy is never called,\n"
        "  and code inside that library now runs against a build it was not\n"
        "  linked against.\n"
        "  Ways out, in the order they apply:\n"
        "    1. stop one side from providing it — usually the package that\n"
        "       ships a copy of a library the graph already builds;\n"
        "    2. make both resolve to ONE file: declare the library's real\n"
        "       SONAME on its target, so the shared copy and the built one are\n"
        "       the same name;\n"
        "    3. `[build] dependency_linkage` / a per-dependency `linkage`\n"
        "       changes which form mcpp builds — it removes THIS finding, but\n"
        "       it only unifies the two providers when (2) holds as well.";
    return body;
}

} // namespace mcpp::build::symbol_provision
