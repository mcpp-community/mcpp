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
#include <string>
#include <vector>

import mcpp.toolchain.predefines;

namespace pd = mcpp::toolchain::predefines;

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
              "-D__mcpp_target_freebsd__=1");
    EXPECT_EQ(pd::define_tokens("none", false).front(),
              "-D__mcpp_target_none__=1");
}

TEST(Predefines, OneMacroPerTargetAndItIsAlwaysPresent) {
    // Conditional emission would make absence ambiguous: "not Windows" and
    // "Windows, but nothing suppressed its macros" would read the same.
    for (auto const& os : {"linux", "windows", "macos", "ios", "android", "none"}) {
        auto ms = emitted(os, false);
        const auto n = std::ranges::count_if(ms, [](std::string const& m) {
            return m.starts_with("__mcpp_target_");
        });
        EXPECT_EQ(n, 1) << "os=" << os;
    }
}

TEST(Predefines, OpenkalIsTiedToTheResolvedLayerAndNotToAnyTarget) {
    for (auto const& os : {"linux", "windows", "macos"}) {
        auto without = emitted(os, false);
        auto with    = emitted(os, true);
        EXPECT_EQ(std::ranges::find(without, "__openkal__"), without.end());
        EXPECT_NE(std::ranges::find(with, "__openkal__"), with.end());
    }
}

TEST(Predefines, AnEmptyTargetOsEmitsNoTargetMacro) {
    // A triple that failed to parse must not produce `__mcpp_target___`.
    auto ms = emitted("", false);
    EXPECT_TRUE(ms.empty());
}

TEST(Predefines, EveryContractRowStatesBothWhatIsAllowedAndWhatIsNot) {
    // A rule with only a permission invites every use its author did not
    // think of; the `__openkal__` entry has had a forbidden list since it was
    // written, and the rest follow it.
    for (auto const& e : pd::kContract) {
        EXPECT_FALSE(e.allowed.empty())   << e.spelling;
        EXPECT_FALSE(e.forbidden.empty()) << e.spelling;
    }
}

TEST(Predefines, EveryOwnedNameCarriesTheMcppPrefix) {
    // A name mcpp owns means what mcpp says it means. A name it merely
    // supplies -- `__unix__` -- is the standard one and must NOT be renamed
    // into this project's namespace.
    for (auto const& e : pd::kContract) {
        if (e.owned)
            EXPECT_TRUE(e.spelling.starts_with("__mcpp_") || e.spelling == "__openkal__")
                << e.spelling << " is owned and is spelt outside the namespace";
        else
            EXPECT_FALSE(e.spelling.starts_with("__mcpp_"))
                << e.spelling << " is supplied rather than owned, so it keeps "
                   "the standard spelling";
    }
}
