
import std;
import mcpplibs.mtest;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

namespace ts = mcpp::toolchain::target_set;
namespace tt = mcpp::toolchain::triple;

namespace {

// The engine's own conventions for every candidate, except where the target
// table's platform column names one.
std::expected<std::string, std::string> column_platform(const tt::Triple& t) {
    if (auto* row = tt::find_known_target(t)) return std::string(row->platform);
    return std::string{};
}

std::vector<std::string> legs_of(std::initializer_list<std::string> requests) {
    std::vector<std::string> req(requests);
    auto r = ts::resolve(req, column_platform);
    if (auto&& failed = expect(r.has_value()); !failed) failed << (r ? "" : r.error());
    std::vector<std::string> out;
    if (r) for (auto& l : *r) out.push_back(l.triple);
    return out;
}

} // namespace

// ── recognition: the first segment, not a failed parse ──────────────────────

UTest recognises_system_words{"TargetSet/RecognisesSystemWords", [] {
    expect(ts::is_set_spelling("openkal"));
    expect(ts::is_set_spelling("linux-musl"));
    expect(ts::is_set_spelling("none-eabihf"));
    expect(ts::is_set_spelling("android"));
    expect(!ts::is_set_spelling("x86_64-openkal"));
    expect(!ts::is_set_spelling("aarch64-linux-musl"));
}};

UTest vendor_first_spelling_is_a_triple{"TargetSet/VendorFirstSpellingIsATriple", [] {
    // `unknown-linux-gnu` parses as a triple (architecture `unknown`); it is
    // not a set, because `unknown` is not a system word.
    expect(!ts::is_set_spelling("unknown-linux-gnu"));
    expect(!ts::is_set_spelling("apple-macos"));
    auto legs = legs_of({"unknown-linux-gnu"});
    expect(fatal(eq(legs.size(), 1u)));
    expect(eq(legs.front(), "unknown-linux-gnu"));
}};

// ── expansion ────────────────────────────────────────────────────────────────

UTest openkal_names_both_architectures{"TargetSet/OpenkalNamesBothArchitectures", [] {
    auto e = ts::expand_set("openkal", column_platform);
    if (auto&& failed = expect(fatal(e.has_value())); !failed) failed << e.error();
    expect(eq(e->members, (std::vector<std::string>{"x86_64-openkal", "aarch64-openkal"})));
    expect(e->platform.starts_with("mcpplibs.openkal-platform@"));
}};

UTest linux_musl_leaves_planned_rows_out{"TargetSet/LinuxMuslLeavesPlannedRowsOut", [] {
    auto e = ts::expand_set("linux-musl", column_platform);
    if (auto&& failed = expect(fatal(e.has_value())); !failed) failed << e.error();
    expect(eq(e->members, (std::vector<std::string>{"x86_64-linux-musl", "aarch64-linux-musl"})));
    for (auto& m : e->members) expect(ne(m, "riscv64-linux-musl"));
}};

UTest linux_fills_gnu_and_leaves_android_out{"TargetSet/LinuxFillsGnuAndLeavesAndroidOut", [] {
    auto e = ts::expand_set("linux", column_platform);
    if (auto&& failed = expect(fatal(e.has_value())); !failed) failed << e.error();
    expect(eq(e->members, (std::vector<std::string>{"x86_64-linux-gnu", "aarch64-linux-gnu"})));
}};

UTest windows_fills_gnu{"TargetSet/WindowsFillsGnu", [] {
    auto e = ts::expand_set("windows", column_platform);
    if (auto&& failed = expect(fatal(e.has_value())); !failed) failed << e.error();
    expect(eq(e->members, (std::vector<std::string>{"x86_64-windows-gnu"})));
}};

UTest android_is_the_linux_android_rows{"TargetSet/AndroidIsTheLinuxAndroidRows", [] {
    auto e = ts::expand_set("android", column_platform);
    if (auto&& failed = expect(fatal(e.has_value())); !failed) failed << e.error();
    expect(eq(e->members, (std::vector<std::string>{"aarch64-linux-android", "x86_64-linux-android"})));
}};

UTest ios_and_its_simulator{"TargetSet/IosAndItsSimulator", [] {
    auto device = ts::expand_set("ios", column_platform);
    if (auto&& failed = expect(fatal(device.has_value())); !failed) failed << device.error();
    expect(eq(device->members, (std::vector<std::string>{"aarch64-ios"})));
    auto sim = ts::expand_set("ios-sim", column_platform);
    if (auto&& failed = expect(fatal(sim.has_value())); !failed) failed << sim.error();
    expect(eq(sim->members, (std::vector<std::string>{"aarch64-ios-sim", "x86_64-ios-sim"})));
}};

UTest bare_metal_needs_its_environment{"TargetSet/BareMetalNeedsItsEnvironment", [] {
    auto bare = ts::expand_set("none", column_platform);
    expect(fatal(!bare.has_value()));
    expect(ne(bare.error().find("none-elf"), std::string::npos));
    auto elf = ts::expand_set("none-elf", column_platform);
    if (auto&& failed = expect(fatal(elf.has_value())); !failed) failed << elf.error();
    expect(eq(elf->members, (std::vector<std::string>{
        "riscv64-none-elf", "riscv32-none-elf", "aarch64-none-elf", "x86_64-none-elf",
        "i686-none-elf"})));
}};

UTest empty_set_is_refused_naming_the_planned_rows{"TargetSet/EmptySetIsRefusedNamingThePlannedRows", [] {
    auto e = ts::expand_set("macos-gnu", column_platform);
    expect(fatal(!e.has_value()));
    auto none = ts::expand_set("linux-msvc", column_platform);
    expect(fatal(!none.has_value()));
    expect(ne(none.error().find("names no target"), std::string::npos));
}};

UTest malformed_sets_are_refused{"TargetSet/MalformedSetsAreRefused", [] {
    expect(!(ts::expand_set("linux-musl-extra", column_platform).has_value()));
    expect(!(ts::expand_set("linux-", column_platform).has_value()));
    expect(!(ts::expand_set("openkal-gnu", column_platform).has_value()));
    expect(!(ts::expand_set("android-x", column_platform).has_value()));
}};

UTest members_selecting_different_platforms_are_refused{"TargetSet/MembersSelectingDifferentPlatformsAreRefused", [] {
    auto split = [](const tt::Triple& t) -> std::expected<std::string, std::string> {
        return t.arch == "x86_64" ? std::string("a@1") : std::string("b@1");
    };
    auto e = ts::expand_set("openkal", split);
    expect(fatal(!e.has_value()));
    expect(ne(e.error().find("select different platforms"), std::string::npos));
    expect(ne(e.error().find("x86_64-openkal"), std::string::npos));
    expect(ne(e.error().find("aarch64-openkal"), std::string::npos));
}};

UTest a_selection_error_names_the_member{"TargetSet/ASelectionErrorNamesTheMember", [] {
    auto broken = [](const tt::Triple&) -> std::expected<std::string, std::string> {
        return std::unexpected(std::string("no such platform"));
    };
    auto e = ts::expand_set("openkal", broken);
    expect(fatal(!e.has_value()));
    expect(ne(e.error().find("x86_64-openkal: no such platform"), std::string::npos));
}};

// ── several --target ─────────────────────────────────────────────────────────

UTest union_in_table_order_without_duplicates{"TargetSet/UnionInTableOrderWithoutDuplicates", [] {
    auto legs = legs_of({"aarch64-openkal", "openkal", "x86_64-linux-musl"});
    expect(eq(legs, (std::vector<std::string>{
        "x86_64-linux-musl", "x86_64-openkal", "aarch64-openkal"})));
}};

UTest a_target_keeps_its_spelling{"TargetSet/ATargetKeepsItsSpelling", [] {
    // Planned as the command would plan it alone: `x86_64-linux` is completed
    // and reported by the build, not here.
    auto legs = legs_of({"x86_64-linux"});
    expect(fatal(eq(legs.size(), 1u)));
    expect(eq(legs.front(), "x86_64-linux"));
    auto twice = legs_of({"x86_64-linux", "x86_64-linux-gnu"});
    expect(eq(twice.size(), 1u));
}};

UTest unknown_targets_follow_the_table_in_written_order{"TargetSet/UnknownTargetsFollowTheTableInWrittenOrder", [] {
    auto legs = legs_of({"zz-custom-target", "x86_64-openkal", "yy-other"});
    expect(eq(legs, (std::vector<std::string>{"x86_64-openkal", "zz-custom-target", "yy-other"})));
}};

UTest legs_record_their_set{"TargetSet/LegsRecordTheirSet", [] {
    std::vector<std::string> req{"openkal", "x86_64-linux-musl"};
    auto r = ts::resolve(req, column_platform);
    expect(fatal(r.has_value()));
    for (auto& l : *r) {
        if (l.triple.ends_with("openkal")) expect(eq(l.fromSet, "openkal"));
        else expect(l.fromSet.empty());
    }
}};
