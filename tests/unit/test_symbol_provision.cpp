// One library, one provider — evaluated on a produced image (issue #519).
//
// Two of these are regression tests for predicates that were WRONG before
// they were measured, and both would have failed silently rather than loudly:
//
//   * matching copy relocations by NAME reports `environ` as hijacked in
//     every dynamically linked executable, because it is a weak alias of
//     `__environ` at the same address and only the latter is in the
//     relocation table;
//   * reporting on stage one alone flags mcpp's OWN `kind = "shared"`
//     arrangement, where a shared dependency's static dependency legitimately
//     lands in the consumer's executable with exactly one copy in the process.

#include <gtest/gtest.h>

import std;
import mcpp.build.symbol_provision;
import mcpp.runtime.elf;

namespace sp = mcpp::build::symbol_provision;
namespace elf = mcpp::platform::elf;

namespace {

elf::DynamicSymbols image() {
    elf::DynamicSymbols s;
    s.present = true;
    s.copyRelocationsKnown = true;
    return s;
}

elf::DynamicSymbol func(std::string name, std::uint64_t at = 0x1000) {
    return elf::DynamicSymbol{ .name = std::move(name), .isFunc = true, .value = at };
}

elf::DynamicSymbol object(std::string name, std::uint64_t at) {
    return elf::DynamicSymbol{ .name = std::move(name), .isFunc = false, .value = at };
}

std::vector<std::string> names(const std::vector<sp::Export>& exports) {
    std::vector<std::string> out;
    for (auto const& e : exports) out.push_back(e.name);
    return out;
}

} // namespace

// ── stage one ──────────────────────────────────────────────────────────────

TEST(SymbolProvision, ADefinedFunctionIsAlwaysAnExport) {
    auto s = image();
    s.defined.push_back(func("inflate"));
    // Even at an address that carries a copy relocation: a copy relocation
    // moves DATA, so a function sharing that address is not one.
    s.copyRelocations.insert(0x1000);
    auto exports = sp::exported_definitions(s);
    ASSERT_TRUE(exports.has_value());
    EXPECT_EQ(names(*exports), std::vector<std::string>{"inflate"});
}

TEST(SymbolProvision, CopyRelocatedDataIsNotAnExport) {
    auto s = image();
    s.defined.push_back(object("stdout", 0xb1d888));
    s.copyRelocations.insert(0xb1d888);
    auto exports = sp::exported_definitions(s);
    ASSERT_TRUE(exports.has_value());
    EXPECT_TRUE(exports->empty());
}

TEST(SymbolProvision, CopyRelocationsMatchByAddressNotByName) {
    // MEASURED, on every mcpp binary: glibc's `environ` is a WEAK alias of
    // `__environ` at one address, and only `__environ` appears in `.rela.dyn`.
    // A name-keyed filter reports `environ` as a hijacked symbol in every
    // dynamically linked executable ever built.
    auto s = image();
    s.defined.push_back(object("__environ", 0xb1d840));
    s.defined.push_back(object("environ",   0xb1d840));   // same address
    s.copyRelocations.insert(0xb1d840);                   // only one entry
    auto exports = sp::exported_definitions(s);
    ASSERT_TRUE(exports.has_value());
    EXPECT_TRUE(exports->empty()) << "environ must not be reported";
}

TEST(SymbolProvision, AnUninitialisedDataSymbolWithNoCopyRelocationIsAnExport) {
    // /usr/bin/ls's `obstack_alloc_failed_handler` — a function-pointer
    // variable that gnulib defines and glibc also defines. It lives in .bss
    // like a copy relocation would, and it is NOT one.
    auto s = image();
    s.defined.push_back(object("obstack_alloc_failed_handler", 0x2000));
    auto exports = sp::exported_definitions(s);
    ASSERT_TRUE(exports.has_value());
    EXPECT_EQ(names(*exports),
              std::vector<std::string>{"obstack_alloc_failed_handler"});
}

TEST(SymbolProvision, AnUnknownMachineDeclinesRatherThanGuessing) {
    // Without the machine's COPY relocation type every data symbol looks like
    // an export. "Could not evaluate" must not be spelled the same way as
    // "evaluated and clean".
    auto s = image();
    s.copyRelocationsKnown = false;
    s.defined.push_back(object("stdout", 0x10));
    EXPECT_FALSE(sp::exported_definitions(s).has_value());
}

// ── stage two ──────────────────────────────────────────────────────────────

TEST(SymbolProvision, AnExportWithNoSecondProviderIsNotAConflict) {
    // mcpp's OWN arrangement. A `kind = "shared"` dependency's link unit
    // takes only its own objects, so its static dependency lands in the
    // consumer's executable and the shared library binds back to it. One copy
    // in the process, entirely benign — and stage one alone would warn about
    // it on every build that uses compat.x11 with a real static dependency.
    std::vector<sp::Export> exports{ sp::Export{"shared_answer", true} };
    std::vector<sp::Provider> closure{
        sp::Provider{"/lib/libc.so.6", {"printf", "malloc"}},
    };
    EXPECT_TRUE(sp::conflicting_exports(exports, closure).empty());
}

TEST(SymbolProvision, AnExportWithASecondProviderIsAConflictAndNamesIt) {
    std::vector<sp::Export> exports{ sp::Export{"inflate", true},
                                     sp::Export{"deflate", true} };
    std::vector<sp::Provider> closure{
        sp::Provider{"/pkg/lib/libz.so.1", {"deflate", "inflate", "crc32"}},
        sp::Provider{"/lib/libc.so.6",     {"printf"}},
    };
    auto conflicts = sp::conflicting_exports(exports, closure);
    ASSERT_EQ(conflicts.size(), 2u);
    EXPECT_EQ(conflicts[0].name, "inflate");
    ASSERT_EQ(conflicts[0].alsoProvidedBy.size(), 1u);
    EXPECT_EQ(conflicts[0].alsoProvidedBy[0], "/pkg/lib/libz.so.1");
}

TEST(SymbolProvision, TheReportNamesEveryProviderAndCapsTheSymbolList) {
    sp::Report report;
    report.status = sp::Status::Conflict;
    report.total = 217;
    for (int i = 0; i < 20; ++i)
        report.conflicts.push_back(sp::Conflict{
            std::format("sym{}", i), true, {"/pkg/lib/libz.so.1"}});
    report.exported = report.conflicts.size();

    auto text = report.explain("consumer");
    EXPECT_NE(text.find("20 symbols"), std::string::npos);
    EXPECT_NE(text.find("and 14 more"), std::string::npos);
    EXPECT_NE(text.find("/pkg/lib/libz.so.1"), std::string::npos);
    // The three ways out are the point of the message, and their ORDER is
    // load-bearing: switching the form removes this finding while leaving two
    // copies loaded unless the SONAMEs also match, so it must not be first.
    auto stop = text.find("stop one side");
    auto soname = text.find("SONAME");
    auto form = text.find("dependency_linkage");
    ASSERT_NE(stop, std::string::npos);
    ASSERT_NE(soname, std::string::npos);
    ASSERT_NE(form, std::string::npos);
    EXPECT_LT(stop, soname);
    EXPECT_LT(soname, form);
}

TEST(SymbolProvision, OnlyAConflictIsActionable) {
    EXPECT_FALSE(sp::not_applicable("static").actionable());
    EXPECT_FALSE(sp::not_evaluated("unknown machine").actionable());
    sp::Report clean; clean.status = sp::Status::Clean;
    EXPECT_FALSE(clean.actionable());
    EXPECT_TRUE(clean.explain("x").empty());
}

TEST(SymbolProvision, ANonAnswerCarriesItsReason) {
    // "Not checked" and "checked and clean" must never render the same.
    auto na = sp::not_applicable("statically linked");
    EXPECT_EQ(na.status, sp::Status::NotApplicable);
    EXPECT_EQ(na.reason, "statically linked");
    EXPECT_EQ(sp::to_string(sp::Status::NotApplicable), "not-applicable");
    EXPECT_EQ(sp::to_string(sp::Status::NotEvaluated), "not-evaluated");
    EXPECT_NE(sp::to_string(sp::Status::Clean),
              sp::to_string(sp::Status::NotApplicable));
}

// ── the precondition ───────────────────────────────────────────────────────

TEST(SymbolProvision, AnAuthorRequestedExportSurfaceVoidsThePredicate) {
    // /usr/bin/bash exports 2339 symbols on purpose, for loadable builtins.
    // Detecting exactly the thing the author asked for is not a finding.
    EXPECT_TRUE(sp::export_dynamic_requested(
        std::vector<std::string>{"-O2", "-rdynamic"}));
    EXPECT_TRUE(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--export-dynamic"}));
    EXPECT_TRUE(sp::export_dynamic_requested(std::vector<std::string>{"-Wl,-E"}));
    EXPECT_TRUE(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--dynamic-list=syms.txt"}));
    EXPECT_TRUE(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--export-dynamic-symbol=foo"}));
}

TEST(SymbolProvision, OrdinaryLinkFlagsDoNotVoidThePredicate) {
    // A substring sweep would fire on any of these.
    EXPECT_FALSE(sp::export_dynamic_requested(std::vector<std::string>{
        "-O2", "-Wl,-rpath,$ORIGIN", "-lz", "-Wl,--as-needed",
        "-Wl,--enable-new-dtags", "-static-libstdc++", "-shared"}));
}
