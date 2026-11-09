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


import std;
import mcpplibs.mtest;
import mcpp.build.symbol_provision;
import mcpp.runtime.elf;

using namespace mcpplibs::mtest;

namespace sp = mcpp::build::symbol_provision;
namespace elf = mcpp::runtime::elf;

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

UTest a_defined_function_is_always_an_export{"SymbolProvision/ADefinedFunctionIsAlwaysAnExport", [] {
    auto s = image();
    s.defined.push_back(func("inflate"));
    // Even at an address that carries a copy relocation: a copy relocation
    // moves DATA, so a function sharing that address is not one.
    s.copyRelocations.insert(0x1000);
    auto exports = sp::exported_definitions(s);
    expect(fatal(exports.has_value()));
    expect(eq(names(*exports), std::vector<std::string>{"inflate"}));
}};

UTest copy_relocated_data_is_not_an_export{"SymbolProvision/CopyRelocatedDataIsNotAnExport", [] {
    auto s = image();
    s.defined.push_back(object("stdout", 0xb1d888));
    s.copyRelocations.insert(0xb1d888);
    auto exports = sp::exported_definitions(s);
    expect(fatal(exports.has_value()));
    expect(exports->empty());
}};

UTest copy_relocations_match_by_address_not_by_name{"SymbolProvision/CopyRelocationsMatchByAddressNotByName", [] {
    // MEASURED, on every mcpp binary: glibc's `environ` is a WEAK alias of
    // `__environ` at one address, and only `__environ` appears in `.rela.dyn`.
    // A name-keyed filter reports `environ` as a hijacked symbol in every
    // dynamically linked executable ever built.
    auto s = image();
    s.defined.push_back(object("__environ", 0xb1d840));
    s.defined.push_back(object("environ",   0xb1d840));   // same address
    s.copyRelocations.insert(0xb1d840);                   // only one entry
    auto exports = sp::exported_definitions(s);
    expect(fatal(exports.has_value()));
    expect(exports->empty()) << "environ must not be reported";
}};

UTest an_uninitialised_data_symbol_with_no_copy_relocation_is_an_export{"SymbolProvision/AnUninitialisedDataSymbolWithNoCopyRelocationIsAnExport", [] {
    // /usr/bin/ls's `obstack_alloc_failed_handler` — a function-pointer
    // variable that gnulib defines and glibc also defines. It lives in .bss
    // like a copy relocation would, and it is NOT one.
    auto s = image();
    s.defined.push_back(object("obstack_alloc_failed_handler", 0x2000));
    auto exports = sp::exported_definitions(s);
    expect(fatal(exports.has_value()));
    expect(eq(names(*exports), std::vector<std::string>{"obstack_alloc_failed_handler"}));
}};

UTest an_unknown_machine_declines_rather_than_guessing{"SymbolProvision/AnUnknownMachineDeclinesRatherThanGuessing", [] {
    // Without the machine's COPY relocation type every data symbol looks like
    // an export. "Could not evaluate" must not be spelled the same way as
    // "evaluated and clean".
    auto s = image();
    s.copyRelocationsKnown = false;
    s.defined.push_back(object("stdout", 0x10));
    expect(!(sp::exported_definitions(s).has_value()));
}};

// ── stage two ──────────────────────────────────────────────────────────────

UTest an_export_with_no_second_provider_is_not_a_conflict{"SymbolProvision/AnExportWithNoSecondProviderIsNotAConflict", [] {
    // mcpp's OWN arrangement. A `kind = "shared"` dependency's link unit
    // takes only its own objects, so its static dependency lands in the
    // consumer's executable and the shared library binds back to it. One copy
    // in the process, entirely benign — and stage one alone would warn about
    // it on every build that uses compat.x11 with a real static dependency.
    std::vector<sp::Export> exports{ sp::Export{"shared_answer", true} };
    std::vector<sp::Provider> closure{
        sp::Provider{"/lib/libc.so.6", {"printf", "malloc"}},
    };
    expect(sp::conflicting_exports(exports, closure).empty());
}};

UTest an_export_with_a_second_provider_is_a_conflict_and_names_it{"SymbolProvision/AnExportWithASecondProviderIsAConflictAndNamesIt", [] {
    std::vector<sp::Export> exports{ sp::Export{"inflate", true},
                                     sp::Export{"deflate", true} };
    std::vector<sp::Provider> closure{
        sp::Provider{"/pkg/lib/libz.so.1", {"deflate", "inflate", "crc32"}},
        sp::Provider{"/lib/libc.so.6",     {"printf"}},
    };
    auto conflicts = sp::conflicting_exports(exports, closure);
    expect(fatal(eq(conflicts.size(), 2u)));
    expect(eq(conflicts[0].name, "inflate"));
    expect(fatal(eq(conflicts[0].alsoProvidedBy.size(), 1u)));
    expect(eq(conflicts[0].alsoProvidedBy[0], "/pkg/lib/libz.so.1"));
}};

UTest the_report_names_every_provider_and_caps_the_symbol_list{"SymbolProvision/TheReportNamesEveryProviderAndCapsTheSymbolList", [] {
    sp::Report report;
    report.status = sp::Status::Conflict;
    report.total = 217;
    for (int i = 0; i < 20; ++i)
        // DESIGNATED, not positional. A field added to `Conflict` between
        // `isFunc` and `alsoProvidedBy` bound the provider list to a bool
        // here -- a string literal converts to one, so it compiled, and the
        // provider list silently became empty.
        report.conflicts.push_back(sp::Conflict{
            .name = std::format("sym{}", i),
            .isFunc = true,
            .alsoProvidedBy = {"/pkg/lib/libz.so.1"}});
    report.exported = report.conflicts.size();

    auto text = report.explain("consumer");
    expect(ne(text.find("20 symbols"), std::string::npos));
    expect(ne(text.find("and 14 more"), std::string::npos));
    expect(ne(text.find("/pkg/lib/libz.so.1"), std::string::npos));
    // The three ways out are the point of the message, and their ORDER is
    // load-bearing: switching the form removes this finding while leaving two
    // copies loaded unless the SONAMEs also match, so it must not be first.
    auto stop = text.find("stop one side");
    auto soname = text.find("SONAME");
    auto form = text.find("dependency_linkage");
    expect(fatal(ne(stop, std::string::npos)));
    expect(fatal(ne(soname, std::string::npos)));
    expect(fatal(ne(form, std::string::npos)));
    expect(lt(stop, soname));
    expect(lt(soname, form));
}};

UTest only_a_conflict_is_actionable{"SymbolProvision/OnlyAConflictIsActionable", [] {
    expect(!(sp::not_applicable("static").actionable()));
    expect(!(sp::not_evaluated("unknown machine").actionable()));
    sp::Report clean; clean.status = sp::Status::Clean;
    expect(!clean.actionable());
    expect(clean.explain("x").empty());
}};

UTest a_non_answer_carries_its_reason{"SymbolProvision/ANonAnswerCarriesItsReason", [] {
    // "Not checked" and "checked and clean" must never render the same.
    auto na = sp::not_applicable("statically linked");
    expect(eq(na.status, sp::Status::NotApplicable));
    expect(eq(na.reason, "statically linked"));
    expect(eq(sp::to_string(sp::Status::NotApplicable), "not-applicable"));
    expect(eq(sp::to_string(sp::Status::NotEvaluated), "not-evaluated"));
    expect(ne(sp::to_string(sp::Status::Clean), sp::to_string(sp::Status::NotApplicable)));
}};

// ── the precondition ───────────────────────────────────────────────────────

UTest an_author_requested_export_surface_voids_the_predicate{"SymbolProvision/AnAuthorRequestedExportSurfaceVoidsThePredicate", [] {
    // /usr/bin/bash exports 2339 symbols on purpose, for loadable builtins.
    // Detecting exactly the thing the author asked for is not a finding.
    expect(bool(sp::export_dynamic_requested(
        std::vector<std::string>{"-O2", "-rdynamic"})));
    expect(bool(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--export-dynamic"})));
    expect(bool(sp::export_dynamic_requested(std::vector<std::string>{"-Wl,-E"})));
    expect(bool(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--dynamic-list=syms.txt"})));
    expect(bool(sp::export_dynamic_requested(
        std::vector<std::string>{"-Wl,--export-dynamic-symbol=foo"})));
}};

UTest ordinary_link_flags_do_not_void_the_predicate{"SymbolProvision/OrdinaryLinkFlagsDoNotVoidThePredicate", [] {
    // A substring sweep would fire on any of these.
    expect(!sp::export_dynamic_requested(std::vector<std::string>{
        "-O2", "-Wl,-rpath,$ORIGIN", "-lz", "-Wl,--as-needed",
        "-Wl,--enable-new-dtags", "-static-libstdc++", "-shared"}));
}};

// ── vague linkage is not a second provider ─────────────────────────────────
//
// A template instantiation, an inline function or a vtable is emitted into
// every image that needs it and the loader keeps one. That is the C++ ABI
// working, and reporting it names a correct build.
//
// Measured on the SYCL example once the real findings were repaired: of the
// thirty-nine symbols the image still shared with `libsycl.so.9`,
// thirty-seven were `sycl::queue` and `sycl::buffer` instantiations from the
// same headers libsycl was built from -- and the remaining two were the
// island's own `extern "C"` entry points, which libsycl does not define. A
// check that could not tell binding from name reported all of them.

UTest a_weak_definition_is_carried_through_as_weak{"SymbolProvision/AWeakDefinitionIsCarriedThroughAsWeak", [] {
    auto s = image();
    auto weak = func("_ZN4sycl3_V15queueD2Ev");
    weak.isWeak = true;
    s.defined.push_back(weak);
    s.defined.push_back(func("saxpy_device"));
    auto exports = sp::exported_definitions(s);
    expect(fatal(exports.has_value()));
    expect(fatal(eq(exports->size(), 2u)));
    // Sorted by name: the mangled one first.
    expect(bool((*exports)[0].isWeak));
    expect(!((*exports)[1].isWeak));
}};

UTest a_conflict_remembers_whether_its_definition_is_weak{"SymbolProvision/AConflictRemembersWhetherItsDefinitionIsWeak", [] {
    std::vector<sp::Export> exports{
        { .name = "_ZN4sycl3_V15queueD2Ev", .isFunc = true, .isWeak = true },
        { .name = "inflate",                .isFunc = true, .isWeak = false },
    };
    std::vector<sp::Provider> closure{
        { .label = "libsycl.so.9",
          .defines = {"_ZN4sycl3_V15queueD2Ev", "inflate"} },
    };
    auto conflicts = sp::conflicting_exports(exports, closure);
    expect(fatal(eq(conflicts.size(), 2u)));
    // Both are shared; only the binding separates them, and the caller is what
    // decides which one is a finding. Asserted here rather than in the caller
    // so the DATA carries the distinction even if a future caller forgets it.
    expect(bool(conflicts[0].isWeak));
    expect(!(conflicts[1].isWeak));
}};

// #646 F3. GCC emits the static data of an inline entity with STB_GNU_UNIQUE
// (10) so that the loader keeps one copy across RTLD_LOCAL. It is vague linkage
// exactly as STB_WEAK is; reading only the latter reported seven libstdc++
// objects as a program displacing the library.
UTest the_unique_binding_is_vague_linkage_as_the_weak_one_is{"SymbolProvision/TheUniqueBindingIsVagueLinkageAsTheWeakOneIs", [] {
    expect(elf::is_vague_linkage_binding(2));    // STB_WEAK
    expect(elf::is_vague_linkage_binding(10));   // STB_GNU_UNIQUE
    expect(!elf::is_vague_linkage_binding(1));   // STB_GLOBAL
    expect(!elf::is_vague_linkage_binding(0));   // STB_LOCAL
}};

// #646 F3. The std module's initialiser is linked into every C++ image that
// imports `std`, from one object. A provider that defines a name only through
// an object this build linked into both images is removed from that conflict.
UTest a_definition_both_images_take_from_one_object_is_not_a_conflict{"SymbolProvision/ADefinitionBothImagesTakeFromOneObjectIsNotAConflict", [] {
    std::vector<sp::Conflict> conflicts{
        { .name = "_ZGIW3std", .isFunc = true,
          .alsoProvidedBy = {"bin/liblib.so"} },
        { .name = "inflate", .isFunc = true,
          .alsoProvidedBy = {"bin/liblib.so", "/usr/lib/libz.so.1"} },
    };
    const std::map<std::string, std::set<std::string>> shared{
        { "bin/liblib.so", {"_ZGIW3std"} },
    };
    expect(eq(sp::drop_shared_plan_definitions(conflicts, shared), 1u));
    expect(fatal(eq(conflicts.size(), 1u)));
    expect(eq(conflicts[0].name, "inflate"));
    // The library still defines `inflate` from its OWN object, so it stays a
    // provider: attribution is by object, never by the library as a whole.
    expect(eq(conflicts[0].alsoProvidedBy, (std::vector<std::string>{"bin/liblib.so", "/usr/lib/libz.so.1"})));
}};

// The rule is provenance, not a name pattern: an initialiser-shaped name that
// no shared object defines is still a finding.
UTest an_initialiser_shaped_name_without_shared_provenance_is_still_reported{"SymbolProvision/AnInitialiserShapedNameWithoutSharedProvenanceIsStillReported", [] {
    std::vector<sp::Conflict> conflicts{
        { .name = "_ZGIW5other", .isFunc = true, .alsoProvidedBy = {"bin/libother.so"} },
    };
    expect(eq(sp::drop_shared_plan_definitions(conflicts, {}), 0u));
    expect(eq(conflicts.size(), 1u));
}};
