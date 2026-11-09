// The contract and the emission are two halves of one module, and this is
// what keeps them from drifting apart.
//
// `mcpp.toolchain.predefines` states the complete set of macros this engine
// defines (`kContract`) and produces the tokens for one build
// (`define_tokens`). A macro emitted and not listed is a promise nobody can
// rely on; a row listing a macro nothing emits is one a reader waits for
// forever. Both directions are asserted here rather than reviewed by eye,
// because a set compared by reading is a set compared by sampling --- this
// project has already shipped two tables that were "the ones it was missing"
// and were not.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

import mcpplibs.mtest;

import mcpp.toolchain.predefines;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

namespace pd = mcpp::toolchain::predefines;
namespace triple = mcpp::toolchain::triple;

namespace {

// `-D__name__=1` or `-D__name__` -> `__name__`
std::string macro_of(std::string_view token) {
    std::string s(token);
    if (s.starts_with("-D")) s.erase(0, 2);
    if (const auto eq = s.find('='); eq != std::string::npos) s.erase(eq);
    return s;
}

std::vector<std::string> emitted(std::string_view os, bool openkal) {
    std::vector<std::string> out;
    for (auto const& t : pd::define_tokens(os, openkal)) out.push_back(macro_of(t));
    std::ranges::sort(out);
    return out;
}

bool listed(std::string_view name, std::string_view os) {
    return std::ranges::any_of(pd::kContract, [&](pd::Entry const& e) {
        return pd::spelling_for(e, os) == name;
    });
}

} // namespace

UTest every_emitted_macro_is_in_the_contract{"Predefines/EveryEmittedMacroIsInTheContract", [] {
    for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"})
        for (bool openkal : {false, true})
            for (auto const& m : emitted(os, openkal))
                expect(bool(listed(m, os)))
                    << m << " is emitted for os=" << os
                    << " openkal=" << openkal << " and kContract does not list it";
}};

UTest every_contract_row_this_module_emits_is_reachable{"Predefines/EveryContractRowThisModuleEmitsIsReachable", [] {
    // `Realisation` rows are emitted by `mcpp.toolchain.cenv`, deliberately,
    // and are listed here because the contract is about what a READER may
    // encounter rather than about which module wrote it.
    for (auto const& e : pd::kContract) {
        if (e.when == pd::When::Realisation) continue;
        bool reached = false;
        for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"})
            for (bool openkal : {false, true}) {
                auto ms = emitted(os, openkal);
                if (std::ranges::find(ms, pd::spelling_for(e, os)) != ms.end())
                    reached = true;
            }
        expect(bool(reached))
            << e.spelling << " is in kContract and no configuration emits it";
    }
}};

UTest the_target_macro_is_spelt_from_the_triple_field{"Predefines/TheTargetMacroIsSpeltFromTheTripleField", [] {
    // The engine learns no operating-system name: a target added to the
    // triple parser gets its macro without an edit to this module.
    expect(eq(pd::define_tokens("freebsd", false).front(), "-D__MCPP_TARGET_FREEBSD__=1"));
    expect(eq(pd::define_tokens("none", false).front(), "-D__MCPP_TARGET_NONE__=1"));
}};

UTest one_macro_per_target_and_it_is_always_present{"Predefines/OneMacroPerTargetAndItIsAlwaysPresent", [] {
    // Conditional emission would make absence ambiguous: "not Windows" and
    // "Windows, but nothing suppressed its macros" would read the same.
    for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"}) {
        auto ms = emitted(os, false);
        const auto n = std::ranges::count_if(ms, [](std::string const& m) {
            return m.starts_with("__MCPP_TARGET_");
        });
        expect(eq(n, 1)) << "os=" << os;
    }
}};

UTest openkal_is_tied_to_the_resolved_layer_and_not_to_any_target{"Predefines/OpenkalIsTiedToTheResolvedLayerAndNotToAnyTarget", [] {
    for (auto const& os : {"linux", "windows", "macos"}) {
        auto without = emitted(os, false);
        auto with    = emitted(os, true);
        expect(eq(std::ranges::find(without, "__OPENKAL__"), without.end()));
        expect(ne(std::ranges::find(with, "__OPENKAL__"), with.end()));
    }
}};

UTest an_empty_target_os_emits_no_target_macro{"Predefines/AnEmptyTargetOsEmitsNoTargetMacro", [] {
    // A triple that failed to parse must not produce `__MCPP_TARGET___`.
    auto ms = emitted("", false);
    expect(ms.empty());
}};

UTest every_contract_row_states_both_what_is_allowed_and_what_is_not{"Predefines/EveryContractRowStatesBothWhatIsAllowedAndWhatIsNot", [] {
    // A rule with only a permission invites every use its author did not
    // think of; the `__OPENKAL__` entry has had a forbidden list since it was
    // written, and the rest follow it.
    for (auto const& e : pd::kContract) {
        expect(!e.allowed.empty())   << e.spelling;
        expect(!e.forbidden.empty()) << e.spelling;
    }
}};

UTest every_owned_name_is_spelt_in_this_projects_convention{"Predefines/EveryOwnedNameIsSpeltInThisProjectsConvention", [] {
    // Upper case, `__`-wrapped. The convention splits by what a name IS: a
    // vendor or product name is upper (`__APPLE__`, `_WIN32`), a
    // kind-of-system name is lower (`__linux__`). Every owned row names a
    // vendor or a product --- mcpp itself, or openkal --- so every one of
    // them is upper.
    //
    // THE PREFIX IS NOT THE INVARIANT, and asserting it was would be wrong:
    // `__OPENKAL__` is owned and names openkal, not mcpp. What every owned
    // row shares is the spelling convention, which is what is checked here.
    // A row mcpp merely SUPPLIES keeps the standard spelling, whatever it is,
    // and must never be pulled into this project's namespace.
    for (auto const& e : pd::kContract) {
        if (!e.owned) {
            expect(!e.spelling.starts_with("__MCPP_"))
                << e.spelling << " is supplied rather than owned, so it keeps "
                   "the standard spelling";
            continue;
        }
        expect(e.spelling.starts_with("__") && e.spelling.ends_with("__"))
            << e.spelling << " is owned and is not `__`-wrapped";
        for (char c : e.spelling)
            expect(!(c >= 'a' && c <= 'z'))
                << e.spelling << " is owned and carries a lower-case letter";
    }
}};

UTest every_target_in_the_registry_yields_a_valid_identifier{"Predefines/EveryTargetInTheRegistryYieldsAValidIdentifier", [] {
    // The spelling is derived from the triple's own `os` field, which is why
    // a target added to the parser needs no edit to `predefines.cppm`. The
    // price of that is that an `os` carrying a character no identifier may
    // hold --- a dot, a dash, a version suffix --- would produce a macro no
    // compiler accepts, and the failure would land in the user's build rather
    // than here.
    //
    // THE DENOMINATOR IS THE REGISTRY, not a list of names written beside
    // this test. A list is a sample, and a sample cannot report the row
    // somebody adds next year.
    std::size_t rows = 0;
    for (auto const& row : triple::known_targets()) {
        const auto t = triple::parse(row.canonical);
        expect(fatal(t.has_value())) << row.canonical << " does not parse";
        const auto toks = pd::define_tokens(t->os, false);
        if (auto&& failed = expect(fatal(eq(toks.size(), 1u))); !failed) failed << row.canonical << " (os=" << t->os << ")";
        const std::string m = macro_of(toks.front());
        expect(!m.empty());
        if (auto&& failed = expect(std::isalpha(static_cast<unsigned char>(m.front()))
                    || m.front() == '_'); !failed) failed << row.canonical << " -> " << m;
        for (char c : m)
            expect(std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                << row.canonical << " -> " << m
                << " is not a valid identifier";
        ++rows;
    }
    // A registry that enumerated nothing would pass every assertion above.
    expect(gt(rows, 10u)) << "the target registry produced " << rows << " rows";
}};
