// Unit tests for the two things `mcpp pack` over several members reads from the
// plan (member selection design 2026-09-30, K1): which member a package acts for
// in a packaging pass (`pack_owner`), and the view of one member of a plan of
// several (`with_member`), which `mcpp test` and `mcpp pack` read a member's
// closure through. The e2e halves are tests/e2e/867_… to 869_….


import std;
import mcpplibs.mtest;
import mcpp.build.plan;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

namespace {

using Names = std::vector<std::string>;

// A plan of two members, `a` and `b`, each with a link group of its own.
mcpp::build::BuildContext two_members() {
    mcpp::build::BuildContext ctx;
    ctx.manifest.package.name = "virtual";
    ctx.projectRoot = "/ws";
    ctx.plan.productDir = "bin";
    ctx.plan.runtimeLibraryDirs = {"/plan/lib"};
    for (std::string_view n : {"a", "b"}) {
        mcpp::build::BuildContext::WorkspaceMember m;
        m.name = std::string(n);
        m.memberPath = std::string(n);
        m.root = std::filesystem::path("/ws") / n;
        m.manifest.package.name = std::string(n);
        ctx.workspaceMembers.push_back(std::move(m));

        mcpp::build::BuildPlan::LinkGroup g;
        g.member = std::string(n);
        g.productDir = std::filesystem::path("bin") / n;
        g.runtimeLibraryDirs = {std::filesystem::path("/closure") / n};
        ctx.plan.linkGroups.push_back(std::move(g));
    }
    // A program shipped through `artifacts` has a group that places nothing,
    // and no member.
    mcpp::build::BuildPlan::LinkGroup linkOnly;
    linkOnly.linkOnly = true;
    linkOnly.runtimeLibraryDirs = {"/shipped"};
    ctx.plan.linkGroups.push_back(std::move(linkOnly));
    return ctx;
}

} // namespace

// A package acts for the member that is itself, the one member that reaches it,
// and none when several do.
UTest a_member_acts_for_itself{"PackOwner/AMemberActsForItself", [] {
    expect(eq(mcpp::build::pack_owner("a", Names{"a", "b"}), "a"));
    expect(eq(mcpp::build::pack_owner("b", Names{"b"}), "b"));
}};

UTest a_package_one_member_reaches_acts_for_that_member{"PackOwner/APackageOneMemberReachesActsForThatMember", [] {
    expect(eq(mcpp::build::pack_owner("dist", Names{"a"}), "a"));
}};

UTest a_package_several_members_reach_acts_for_none{"PackOwner/APackageSeveralMembersReachActsForNone", [] {
    expect(eq(mcpp::build::pack_owner("core", Names{"a", "b"}), ""));
    expect(eq(mcpp::build::pack_owner("orphan", Names{}), ""));
}};

// The view of a member holds the member's closure, manifest and root, and is
// undone when the call returns.
UTest reads_the_members_own_fields_and_restores_the_groups{"WithMember/ReadsTheMembersOwnFieldsAndRestoresTheGroups", [] {
    auto ctx = two_members();
    bool ran = false;
    mcpp::build::with_member(ctx, "b", [&] {
        ran = true;
        expect(eq(ctx.manifest.package.name, "b"));
        expect(eq(ctx.projectRoot, std::filesystem::path("/ws/b")));
        expect(eq(ctx.plan.productDir, std::filesystem::path("bin/b")));
        expect(fatal(eq(ctx.plan.runtimeLibraryDirs.size(), 1u)));
        expect(eq(ctx.plan.runtimeLibraryDirs.front(), std::filesystem::path("/closure/b")));
    });
    expect(bool(ran));
    expect(eq(ctx.manifest.package.name, "virtual"));
    expect(eq(ctx.projectRoot, std::filesystem::path("/ws")));
    expect(eq(ctx.plan.productDir, std::filesystem::path("bin")));
    expect(fatal(eq(ctx.plan.runtimeLibraryDirs.size(), 1u)));
    expect(eq(ctx.plan.runtimeLibraryDirs.front(), std::filesystem::path("/plan/lib")));
    // The groups keep what they held, so the next member reads its own.
    expect(eq(ctx.plan.linkGroups[0].runtimeLibraryDirs.front(), std::filesystem::path("/closure/a")));
    expect(eq(ctx.plan.linkGroups[1].runtimeLibraryDirs.front(), std::filesystem::path("/closure/b")));
    expect(eq(ctx.workspaceMembers[1].manifest.package.name, "b"));
    expect(eq(ctx.workspaceMembers[1].root, std::filesystem::path("/ws/b")));
}};

UTest one_member_after_the_other_each_reads_its_own{"WithMember/OneMemberAfterTheOtherEachReadsItsOwn", [] {
    auto ctx = two_members();
    std::vector<std::string> seen;
    for (std::string_view m : {"a", "b", "a"})
        mcpp::build::with_member(ctx, m, [&] {
            seen.push_back(ctx.plan.runtimeLibraryDirs.front().generic_string());
        });
    expect(eq(seen, (std::vector<std::string>{"/closure/a", "/closure/b", "/closure/a"})));
}};

UTest the_exchange_is_undone_when_the_call_throws{"WithMember/TheExchangeIsUndoneWhenTheCallThrows", [] {
    auto ctx = two_members();
    expect(throws<std::runtime_error>([&] { mcpp::build::with_member(ctx, "a", [&] { throw std::runtime_error("boom"); }); }));
    expect(eq(ctx.manifest.package.name, "virtual"));
    expect(eq(ctx.projectRoot, std::filesystem::path("/ws")));
    expect(eq(ctx.plan.runtimeLibraryDirs.front(), std::filesystem::path("/plan/lib")));
    expect(eq(ctx.plan.linkGroups[0].runtimeLibraryDirs.front(), std::filesystem::path("/closure/a")));
}};

// No owner, a name the plan does not hold, and a group that places nothing all
// read the plan as it is.
UTest a_plan_without_the_member_is_read_as_it_is{"WithMember/APlanWithoutTheMemberIsReadAsItIs", [] {
    auto ctx = two_members();
    for (std::string_view owner : {"", "nosuch"}) {
        bool ran = false;
        mcpp::build::with_member(ctx, owner, [&] {
            ran = true;
            expect(eq(ctx.manifest.package.name, "virtual"));
            expect(eq(ctx.plan.runtimeLibraryDirs.front(), std::filesystem::path("/plan/lib")));
        });
        expect(bool(ran));
    }
    mcpp::build::BuildContext plain;
    plain.manifest.package.name = "solo";
    mcpp::build::with_member(plain, "solo", [&] { expect(eq(plain.manifest.package.name, "solo")); });
    expect(eq(plain.manifest.package.name, "solo"));
}};
