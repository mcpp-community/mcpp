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
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

import mcpp.toolchain.predefines;
import mcpp.toolchain.triple;

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

TEST(Predefines, EveryEmittedMacroIsInTheContract) {
    for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"})
        for (bool openkal : {false, true})
            for (auto const& m : emitted(os, openkal))
                EXPECT_TRUE(listed(m, os))
                    << m << " is emitted for os=" << os
                    << " openkal=" << openkal << " and kContract does not list it";
}

TEST(Predefines, EveryContractRowThisModuleEmitsIsReachable) {
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
        EXPECT_TRUE(reached)
            << e.spelling << " is in kContract and no configuration emits it";
    }
}

TEST(Predefines, TheTargetMacroIsSpeltFromTheTripleField) {
    // The engine learns no operating-system name: a target added to the
    // triple parser gets its macro without an edit to this module.
    EXPECT_EQ(pd::define_tokens("freebsd", false).front(),
              "-D__MCPP_TARGET_FREEBSD__=1");
    EXPECT_EQ(pd::define_tokens("none", false).front(),
              "-D__MCPP_TARGET_NONE__=1");
}

TEST(Predefines, OneMacroPerTargetAndItIsAlwaysPresent) {
    // Conditional emission would make absence ambiguous: "not Windows" and
    // "Windows, but nothing suppressed its macros" would read the same.
    for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"}) {
        auto ms = emitted(os, false);
        const auto n = std::ranges::count_if(ms, [](std::string const& m) {
            return m.starts_with("__MCPP_TARGET_");
        });
        EXPECT_EQ(n, 1) << "os=" << os;
    }
}

TEST(Predefines, OpenkalIsTiedToTheResolvedLayerAndNotToAnyTarget) {
    for (auto const& os : {"linux", "windows", "macos"}) {
        auto without = emitted(os, false);
        auto with    = emitted(os, true);
        EXPECT_EQ(std::ranges::find(without, "__OPENKAL__"), without.end());
        EXPECT_NE(std::ranges::find(with, "__OPENKAL__"), with.end());
    }
}

TEST(Predefines, AnEmptyTargetOsEmitsNoTargetMacro) {
    // A triple that failed to parse must not produce `__MCPP_TARGET___`.
    auto ms = emitted("", false);
    EXPECT_TRUE(ms.empty());
}

TEST(Predefines, EveryContractRowStatesBothWhatIsAllowedAndWhatIsNot) {
    // A rule with only a permission invites every use its author did not
    // think of; the `__OPENKAL__` entry has had a forbidden list since it was
    // written, and the rest follow it.
    for (auto const& e : pd::kContract) {
        EXPECT_FALSE(e.allowed.empty())   << e.spelling;
        EXPECT_FALSE(e.forbidden.empty()) << e.spelling;
    }
}

TEST(Predefines, EveryOwnedNameIsSpeltInThisProjectsConvention) {
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
            EXPECT_FALSE(e.spelling.starts_with("__MCPP_"))
                << e.spelling << " is supplied rather than owned, so it keeps "
                   "the standard spelling";
            continue;
        }
        EXPECT_TRUE(e.spelling.starts_with("__") && e.spelling.ends_with("__"))
            << e.spelling << " is owned and is not `__`-wrapped";
        for (char c : e.spelling)
            EXPECT_FALSE(c >= 'a' && c <= 'z')
                << e.spelling << " is owned and carries a lower-case letter";
    }
}

TEST(Predefines, EveryTargetInTheRegistryYieldsAValidIdentifier) {
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
        ASSERT_TRUE(t.has_value()) << row.canonical << " does not parse";
        const auto toks = pd::define_tokens(t->os, false);
        ASSERT_EQ(toks.size(), 1u) << row.canonical << " (os=" << t->os << ")";
        const std::string m = macro_of(toks.front());
        EXPECT_FALSE(m.empty());
        EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(m.front()))
                    || m.front() == '_')
            << row.canonical << " -> " << m;
        for (char c : m)
            EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                << row.canonical << " -> " << m
                << " is not a valid identifier";
        ++rows;
    }
    // A registry that enumerated nothing would pass every assertion above.
    EXPECT_GT(rows, 10u) << "the target registry produced " << rows << " rows";
}
