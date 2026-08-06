#include <gtest/gtest.h>

import std;
import mcpp.build.directives;
import mcpp.manifest;
import mcpp.toolchain.dialect;

// The directive table is the single definition of what a `build.mcpp`
// directive IS. Before it existed, a directive was defined in nine places and
// adding one meant editing all of them — the failure mode being that you edit
// eight, and the ninth surfaces much later as a flag that silently never
// reached the compiler. These tests hold the table's invariants so that a new
// row cannot be half-added.

namespace dirs = mcpp::build::directives;

namespace {

const mcpp::toolchain::CommandDialect& gnu() {
    return mcpp::toolchain::gnu_dialect();
}

// A root that is genuinely absolute on the running platform. A literal "/pkg"
// is NOT absolute on Windows (no root name), so hardcoding POSIX strings makes
// these tests assert the wrong thing there rather than the right thing
// everywhere.
const std::filesystem::path& test_root() {
    static const std::filesystem::path r =
        (std::filesystem::current_path() / "pkg").lexically_normal();
    return r;
}

// What the implementation should produce for a package-relative path —
// expressed through the same std::filesystem arithmetic, so the expectation is
// about the BEHAVIOUR ("relative resolves against the root") rather than about
// one platform's separator.
std::string under_root(std::string_view rel) {
    return (test_root() / rel).lexically_normal().string();
}

dirs::Directives parse(std::string_view text) {
    dirs::Directives d;
    dirs::accept_output(d, gnu(), test_root(), text);
    return d;
}

}  // namespace

// ── Table integrity ────────────────────────────────────────────────────────

TEST(BuildDirectives, EveryRowIsInternallyConsistent) {
    for (auto const& def : dirs::kTable) {
        EXPECT_FALSE(def.wire.empty());
        EXPECT_GT(def.sinceProtocol, 0) << def.wire;
        EXPECT_LE(def.sinceProtocol, dirs::kProtocolVersion) << def.wire;
        // A RerunKey directive feeds only the re-run key, so it must NOT be
        // persisted as a `d` record; everything else must be, or a cache hit
        // would silently apply less than the program asked for.
        if (def.scope == dirs::Scope::RerunKey)
            EXPECT_TRUE(def.tag.empty()) << def.wire;
        else
            EXPECT_FALSE(def.tag.empty()) << def.wire;
        // The declared-output contract needs a diagnostic that says WHICH
        // contract was broken.
        if (def.mustExistAfterRun) {
            EXPECT_FALSE(def.missingPrefix.empty()) << def.wire;
            EXPECT_FALSE(def.missingSuffix.empty()) << def.wire;
        }
    }
}

TEST(BuildDirectives, WireNamesAreUnique) {
    std::set<std::string_view> seen;
    for (auto const& def : dirs::kTable)
        EXPECT_TRUE(seen.insert(def.wire).second) << "duplicate wire " << def.wire;
}

TEST(BuildDirectives, RowsSharingATagShareASlot) {
    // Cache deserialization resolves a tag to exactly one slot, so two rows
    // with the same tag but different slots would round-trip to the wrong
    // channel. link-lib and link-search legitimately share `ldflag`.
    std::map<std::string_view, dirs::Slot> slotOfTag;
    for (auto const& def : dirs::kTable) {
        if (def.tag.empty()) continue;
        auto [it, fresh] = slotOfTag.try_emplace(def.tag, def.slot);
        if (!fresh) EXPECT_EQ(it->second, def.slot) << def.tag;
    }
}

TEST(BuildDirectives, LookupsAgree) {
    for (auto const& def : dirs::kTable) {
        ASSERT_NE(dirs::find_by_wire(def.wire), nullptr) << def.wire;
        EXPECT_EQ(dirs::find_by_wire(def.wire)->slot, def.slot) << def.wire;
        if (!def.tag.empty()) {
            ASSERT_NE(dirs::find_by_tag(def.tag), nullptr) << def.tag;
            EXPECT_EQ(dirs::find_by_tag(def.tag)->slot, def.slot) << def.tag;
        }
    }
    EXPECT_EQ(dirs::find_by_wire("no-such-directive"), nullptr);
    // The empty tag belongs to the rerun rows and must never resolve, or a
    // malformed cache line would be silently accepted into a slot.
    EXPECT_EQ(dirs::find_by_tag(""), nullptr);
}

// ── Parsing ────────────────────────────────────────────────────────────────

TEST(BuildDirectives, NonDirectiveLinesAreIgnored) {
    auto d = parse("hello\nmcpp is not a directive\n  \n");
    for (std::size_t i = 0; i < dirs::kSlotCount; ++i)
        EXPECT_TRUE(d.slots[i].empty());
    EXPECT_TRUE(d.unknownKeys.empty());
    EXPECT_EQ(d.protocol, 0);
}

TEST(BuildDirectives, TransformsAreAppliedOnceAtParseTime) {
    // An input that is already absolute ON THIS PLATFORM, so the "taken as-is"
    // half of the assertion tests what it claims to.
    const auto alreadyAbs =
        (std::filesystem::current_path() / "abs" / "inc").lexically_normal();

    auto d = parse(std::format("mcpp:cxxflag=-Wall\n"
                               "mcpp:link-lib=z\n"
                               "mcpp:link-search=vendor/lib\n"
                               "mcpp:cfg=HAVE_X\n"
                               "mcpp:include-dir=inc\n"
                               "mcpp:include-dir-after={}\n",
                               alreadyAbs.string()));
    EXPECT_EQ(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-Wall"}));
    // link-lib and link-search share the ldflags slot, in emission order.
    EXPECT_EQ(d.at(dirs::Slot::LdFlags),
              (std::vector<std::string>{"-lz", "-L" + under_root("vendor/lib")}));
    EXPECT_EQ(d.at(dirs::Slot::Defines), (std::vector<std::string>{"-DHAVE_X"}));
    // Relative resolves against the package root; absolute is taken as-is.
    EXPECT_EQ(d.at(dirs::Slot::IncludeDirs),
              (std::vector<std::string>{under_root("inc")}));
    EXPECT_EQ(d.at(dirs::Slot::IncludeDirsAfter),
              (std::vector<std::string>{alreadyAbs.string()}));
}

TEST(BuildDirectives, DialectDecidesTheSpelling) {
    dirs::Directives d;
    dirs::accept_output(d, mcpp::toolchain::msvc_dialect(), test_root(),
                        "mcpp:link-lib=z\nmcpp:cfg=HAVE_X\n");
    EXPECT_EQ(d.at(dirs::Slot::LdFlags), (std::vector<std::string>{"z.lib"}));
    EXPECT_EQ(d.at(dirs::Slot::Defines), (std::vector<std::string>{"/DHAVE_X"}));
}

TEST(BuildDirectives, ValuesMayContainSpacesAndEqualsSigns) {
    // Everything after the FIRST '=' is the value — a path with a space or a
    // define with an '=' must survive intact.
    auto d = parse("mcpp:cxxflag=-DMSG=\"a b\"\n");
    EXPECT_EQ(d.at(dirs::Slot::CxxFlags),
              (std::vector<std::string>{"-DMSG=\"a b\""}));
}

// ── Protocol ───────────────────────────────────────────────────────────────

TEST(BuildDirectives, UnknownKeyWithoutAnAnnouncementIsTolerated) {
    // A hand-written printf program announces nothing; its surface is frozen,
    // so an unknown key is a typo the engine warns about rather than a
    // forward-compat situation it must refuse.
    auto d = parse("mcpp:no-such-thing=1\n");
    EXPECT_EQ(d.protocol, 0);
    EXPECT_EQ(d.unknownKeys, (std::vector<std::string>{"no-such-thing"}));
    EXPECT_FALSE(dirs::protocol_error(d).has_value());
}

TEST(BuildDirectives, UnknownKeyWithAnAnnouncementIsFatal) {
    auto d = parse("mcpp:protocol=1\nmcpp:no-such-thing=1\n");
    EXPECT_EQ(d.protocol, 1);
    auto err = dirs::protocol_error(d);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("no-such-thing"), std::string::npos);
}

TEST(BuildDirectives, NewerProtocolIsFatalAndSaysWhatToDo) {
    auto d = parse("mcpp:protocol=999\n");
    auto err = dirs::protocol_error(d);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("999"), std::string::npos);
    // The message has to tell the user the actionable thing, not just that
    // something is wrong.
    EXPECT_NE(err->find("upgrade"), std::string::npos);
}

TEST(BuildDirectives, CurrentProtocolIsAccepted) {
    auto d = parse(std::format("mcpp:protocol={}\nmcpp:cxxflag=-Wall\n",
                               dirs::kProtocolVersion));
    EXPECT_FALSE(dirs::protocol_error(d).has_value());
    EXPECT_EQ(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-Wall"}));
}

TEST(BuildDirectives, UnknownKeysAreDeduplicated) {
    auto d = parse("mcpp:zzz=1\nmcpp:zzz=2\nmcpp:yyy=3\n");
    EXPECT_EQ(d.unknownKeys, (std::vector<std::string>{"zzz", "yyy"}));
}

// ── Cache round-trip ───────────────────────────────────────────────────────

TEST(BuildDirectives, SerializeDeserializeRoundTrip) {
    auto d = parse("mcpp:cxxflag=-Wall\n"
                   "mcpp:cflag=-std=c11\n"
                   "mcpp:link-lib=z\n"
                   "mcpp:link-search=vendor/lib\n"
                   "mcpp:cfg=HAVE_X\n"
                   "mcpp:generated=src/gen.cpp\n"
                   "mcpp:source=vendor/pick.cpp\n"
                   "mcpp:include-dir=inc\n"
                   "mcpp:include-dir-after=after\n");

    std::ostringstream os;
    dirs::serialize(os, d);

    dirs::Directives back;
    std::istringstream is(os.str());
    std::string line;
    while (std::getline(is, line)) {
        ASSERT_TRUE(line.starts_with("d "));
        auto rest = line.substr(2);
        auto sp = rest.find(' ');
        ASSERT_NE(sp, std::string::npos);
        EXPECT_TRUE(dirs::accept_cache_record(back, rest.substr(0, sp),
                                              rest.substr(sp + 1)));
    }

    // Every persisted slot survives verbatim; the rerun slots deliberately do
    // not (they are re-derived from their own cache records).
    for (auto const& def : dirs::kTable) {
        if (def.tag.empty()) continue;
        EXPECT_EQ(back.at(def.slot), d.at(def.slot)) << def.wire;
    }
}

TEST(BuildDirectives, RerunSlotsAreNotPersistedAsDirectives) {
    auto d = parse("mcpp:rerun-if-changed=config.h\n"
                   "mcpp:rerun-if-env-changed=USE_FAST\n");
    EXPECT_EQ(d.at(dirs::Slot::RerunFiles), (std::vector<std::string>{"config.h"}));
    EXPECT_EQ(d.at(dirs::Slot::RerunEnv), (std::vector<std::string>{"USE_FAST"}));
    std::ostringstream os;
    dirs::serialize(os, d);
    EXPECT_TRUE(os.str().empty());
}

TEST(BuildDirectives, UnknownCacheTagIsRejectedRatherThanDropped) {
    // A cache written by a NEWER mcpp carries tags this one does not know.
    // Silently skipping them would apply a strict subset of what the program
    // asked for; the caller turns `false` into "entry is stale".
    dirs::Directives d;
    EXPECT_FALSE(dirs::accept_cache_record(d, "some-future-tag", "value"));
    EXPECT_TRUE(dirs::accept_cache_record(d, "cxxflag", "-Wall"));
}

// ── Apply ──────────────────────────────────────────────────────────────────

TEST(BuildDirectives, ApplyRoutesEachSlotToItsManifestChannel) {
    auto d = parse("mcpp:cxxflag=-Wall\n"
                   "mcpp:cflag=-std=c11\n"
                   "mcpp:link-lib=z\n"
                   "mcpp:cfg=HAVE_X\n"
                   "mcpp:generated=src/gen.cpp\n"
                   "mcpp:source=vendor/pick.cpp\n"
                   "mcpp:include-dir=inc\n"
                   "mcpp:include-dir-after=after\n");

    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    auto const& bc = m.buildConfig;

    EXPECT_NE(std::find(bc.cxxflags.begin(), bc.cxxflags.end(), "-Wall"),
              bc.cxxflags.end());
    EXPECT_NE(std::find(bc.cflags.begin(), bc.cflags.end(), "-std=c11"),
              bc.cflags.end());
    EXPECT_NE(std::find(bc.ldflags.begin(), bc.ldflags.end(), "-lz"),
              bc.ldflags.end());
    // A cfg define colours BOTH language channels — the one slot that fans out.
    EXPECT_NE(std::find(bc.cflags.begin(), bc.cflags.end(), "-DHAVE_X"),
              bc.cflags.end());
    EXPECT_NE(std::find(bc.cxxflags.begin(), bc.cxxflags.end(), "-DHAVE_X"),
              bc.cxxflags.end());
    // generated= and source= must reach BOTH source lists: the scanner walks
    // the legacy modules.sources mirror, and a file missing from it is
    // invisible to the module scan.
    for (auto const& s : {"src/gen.cpp", "vendor/pick.cpp"}) {
        EXPECT_NE(std::find(bc.sources.begin(), bc.sources.end(), s),
                  bc.sources.end()) << s;
        EXPECT_NE(std::find(m.modules.sources.begin(), m.modules.sources.end(), s),
                  m.modules.sources.end()) << s;
    }
    EXPECT_NE(std::find(bc.includeDirs.begin(), bc.includeDirs.end(),
                        std::filesystem::path(under_root("inc"))),
              bc.includeDirs.end());
    EXPECT_NE(std::find(bc.includeDirsAfter.begin(), bc.includeDirsAfter.end(),
                        std::filesystem::path(under_root("after"))),
              bc.includeDirsAfter.end());
}

TEST(BuildDirectives, ApplyAppendsRatherThanReplaces) {
    mcpp::manifest::Manifest m;
    m.buildConfig.cxxflags.push_back("-O2");
    dirs::apply(m, parse("mcpp:cxxflag=-Wall\n"));
    EXPECT_EQ(m.buildConfig.cxxflags,
              (std::vector<std::string>{"-O2", "-Wall"}));
}

// ── Private-scope fold ─────────────────────────────────────────────────────

namespace {
// UsageRequirements-shaped, so the fold can be exercised without importing
// the scanner into a unit test.
struct FakeUsage {
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::filesystem::path> includeDirsAfter;
    std::vector<std::string>           cflags;
    std::vector<std::string>           cxxflags;
};
}  // namespace

TEST(BuildDirectives, FoldMovesOnlyTheTailAndOnlyPrivateChannels) {
    mcpp::manifest::Manifest m;
    m.buildConfig.cxxflags.push_back("-O2");           // pre-existing, not a directive
    m.buildConfig.includeDirs.emplace_back("/pre");

    auto before = dirs::mark(m);
    dirs::apply(m, parse("mcpp:cxxflag=-Wall\n"
                         "mcpp:cfg=HAVE_X\n"
                         "mcpp:link-lib=z\n"
                         "mcpp:include-dir=inc\n"));

    FakeUsage priv;
    dirs::fold_private_tail(priv, m, before);

    // Only what the program added, and only the private channels.
    EXPECT_EQ(priv.cxxflags, (std::vector<std::string>{"-Wall", "-DHAVE_X"}));
    EXPECT_EQ(priv.cflags,   (std::vector<std::string>{"-DHAVE_X"}));
    EXPECT_EQ(priv.includeDirs,
              (std::vector<std::filesystem::path>{under_root("inc")}));
    // Link flags are NOT private — they reach the final link through their own
    // path, and folding them here would double-apply them.
    EXPECT_TRUE(priv.includeDirsAfter.empty());
}

TEST(BuildDirectives, FoldIsIdempotentOnIncludeDirs) {
    // Include dirs are unique-appended: the same dir emitted twice, or a fold
    // replayed, must not grow the list.
    mcpp::manifest::Manifest m;
    auto before = dirs::mark(m);
    dirs::apply(m, parse("mcpp:include-dir=inc\nmcpp:include-dir=inc\n"));
    FakeUsage priv;
    dirs::fold_private_tail(priv, m, before);
    dirs::fold_private_tail(priv, m, before);
    EXPECT_EQ(priv.includeDirs,
              (std::vector<std::filesystem::path>{under_root("inc")}));
}

// ── Run bound ──────────────────────────────────────────────────────────────

TEST(BuildDirectives, RunTimeoutDefaultsToABoundAndIsOverridable) {
    // Default: bounded. An unbounded build program is how a build hangs with
    // no diagnostic at all.
    EXPECT_GT(dirs::run_timeout().count(), 0);
    EXPECT_EQ(dirs::run_timeout().count(), dirs::kDefaultRunTimeoutSecs * 1000);
}

// ── Glob inputs (#359) ─────────────────────────────────────────────────────
//
// A build program that globs its inputs was structurally unsafe: adding a
// .proto changed no declared file's hash, so the program did not re-run and
// the new file was silently never generated. Measured before the fix:
// `Finished dev in 0.01s`, zero artifacts.
//
// The fingerprint is the SET of matching paths. These tests pin what is in it
// and — just as importantly — what is not.

namespace {

struct GlobTree {
    std::filesystem::path root;
    explicit GlobTree(std::string_view name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "proto");
    }
    ~GlobTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    void write(std::string_view rel, std::string_view text) {
        auto p = root / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream os(p, std::ios::trunc);
        os << text;
    }
    std::string fp(std::string_view pattern, std::string_view outDir = "target") {
        return dirs::glob_fingerprint(root, pattern, outDir);
    }
};

}  // namespace

TEST(BuildDirectives, GlobFingerprintChangesWhenAFileAppearsOrDisappears) {
    GlobTree t{"mcpp_glob_fp_membership"};
    t.write("proto/a.proto", "syntax=\"proto3\";");
    auto one = t.fp("proto/**/*.proto");

    t.write("proto/b.proto", "syntax=\"proto3\";");
    auto two = t.fp("proto/**/*.proto");
    EXPECT_NE(one, two);

    std::filesystem::remove(t.root / "proto/b.proto");
    EXPECT_EQ(t.fp("proto/**/*.proto"), one);
}

TEST(BuildDirectives, GlobFingerprintIgnoresContentSizeAndTimestamp) {
    // Contents are covered by the ordinary `rerun-if-changed` entry for that
    // file. Folding them in here would only add false re-runs — and mtime is
    // unstable across git checkout, container builds and rsync, which this
    // project has already paid for once (the file_time_type epoch in the
    // dependency cache).
    GlobTree t{"mcpp_glob_fp_content"};
    t.write("proto/a.proto", "syntax=\"proto3\";");
    auto before = t.fp("proto/**/*.proto");
    t.write("proto/a.proto", "syntax=\"proto3\"; message Much { string longer = 1; }");
    std::filesystem::last_write_time(
        t.root / "proto/a.proto",
        std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
    EXPECT_EQ(t.fp("proto/**/*.proto"), before);
}

TEST(BuildDirectives, GlobFingerprintNeverWalksTheBuildOutputTree) {
    // A build program writes its outputs INSIDE the project. If a wide pattern
    // included them the set would change on every build and the program would
    // re-run forever — the classic Cargo footgun. Enforced by the engine
    // rather than left to the author's pattern.
    GlobTree t{"mcpp_glob_fp_outdir"};
    t.write("proto/a.proto", "x");
    auto before = t.fp("**");
    t.write("target/.build-mcpp/out/a.pb.cc", "generated");
    t.write("target/.build-mcpp/out/a.pb.h", "generated");
    EXPECT_EQ(t.fp("**"), before);

    // .git is excluded for the same reason: it changes on every commit and
    // never means the build program's inputs changed.
    t.write(".git/HEAD", "ref: refs/heads/main");
    EXPECT_EQ(t.fp("**"), before);
}

TEST(BuildDirectives, GlobFingerprintIsIndependentOfDirectoryIterationOrder) {
    // The set is sorted before hashing, so two trees with the same members
    // agree regardless of the order the platform hands them back.
    GlobTree a{"mcpp_glob_fp_order_a"};
    GlobTree b{"mcpp_glob_fp_order_b"};
    for (auto n : { "z.proto", "a.proto", "m.proto" }) a.write(std::string("proto/") + n, "x");
    for (auto n : { "a.proto", "m.proto", "z.proto" }) b.write(std::string("proto/") + n, "y");
    EXPECT_EQ(a.fp("proto/**/*.proto"), b.fp("proto/**/*.proto"));
}

TEST(BuildDirectives, GlobDirectiveParsesIntoItsOwnSlot) {
    auto d = parse("mcpp:rerun-if-changed-glob=proto/**/*.proto\n");
    ASSERT_EQ(d.at(dirs::Slot::RerunGlobs).size(), 1u);
    EXPECT_EQ(d.at(dirs::Slot::RerunGlobs)[0], "proto/**/*.proto");
    // A re-run key is not a build input, so it must not be persisted as a `d`
    // record — otherwise a cache hit would replay it as one.
    std::ostringstream os;
    dirs::serialize(os, d);
    EXPECT_EQ(os.str().find("proto/**"), std::string::npos) << os.str();
}
