
import std;
import mcpplibs.mtest;
import mcpp.source_kind;
import mcpp.build.stage;

using namespace mcpplibs::mtest;

using namespace mcpp::build::stage;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_stage_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary);
    os << body;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(is), {}};
}

// Retries would only slow the failure tests down; the behaviour under test is
// the decision, not the backoff.
StageOptions no_retry(Verify v = Verify::Content) {
    return StageOptions{.verify = v, .retries = 0, .backoff = std::chrono::milliseconds{0}};
}

}  // namespace

UTest copies_when_destination_missing{"BuildStage/CopiesWhenDestinationMissing", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "nested" / "dir" / "dst.bin";
    write_file(src, "payload");

    auto r = stage_file(src, dst, no_retry());
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << (r ? "" : r.error().message);
    expect(bool(r->copied));
    expect(eq(read_file(dst), "payload"));
}};

// The core of #311: an already-equivalent destination must not be written at
// all — that is what keeps a clangd-mapped std.pcm from failing the build.
UTest equivalent_destination_is_not_touched{"BuildStage/EquivalentDestinationIsNotTouched", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "same-bytes");
    write_file(dst, "same-bytes");

    // Age the destination so any write (or timestamp bump) is observable.
    auto before = std::filesystem::file_time_type::clock::now() - std::chrono::hours{2};
    std::filesystem::last_write_time(dst, before);
    auto recorded = std::filesystem::last_write_time(dst);

    auto r = stage_file(src, dst, no_retry());
    expect(fatal(r.has_value()));
    expect(!r->copied);
    // Not "not bumped to now" — not changed at all. Any mtime change defeats
    // ninja's `restat = 1` and re-triggers the downstream rebuild cascade.
    expect(eq(std::filesystem::last_write_time(dst), recorded));
}};

// Same size, different bytes must be copied by DEFAULT. Staging also carries
// .dll payloads, where PE section padding makes equal sizes across a real
// rebuild ordinary — so size-only equivalence cannot be the default.
UTest same_size_different_bytes_is_copied_by_default{"BuildStage/SameSizeDifferentBytesIsCopiedByDefault", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");

    auto r = stage_file(src, dst, no_retry());
    expect(fatal(r.has_value()));
    expect(bool(r->copied));
    expect(eq(read_file(dst), "AAAA"));
}};

// `--verify size` does not make a readable destination the source's: a
// package's BMI is staged at a path named by the module, so after an upgrade
// the previous version's BMI is there, often at the same size.
UTest same_size_different_bytes_is_copied_under_size_verify{"BuildStage/SameSizeDifferentBytesIsCopiedUnderSizeVerify", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");

    auto r = stage_file(src, dst, no_retry(Verify::Size));
    expect(fatal(r.has_value()));
    expect(bool(r->copied));
    expect(eq(read_file(dst), "AAAA"));
}};

// A destination that cannot be opened is taken as staged under `--verify size`
// when its size is the source's: a holder that denies reading it denies
// replacing it as well.
UTest an_unreadable_same_size_destination_is_skipped_under_size_verify{"BuildStage/AnUnreadableSameSizeDestinationIsSkippedUnderSizeVerify", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "AAAA");
    write_file(dst, "BBBB");
    std::filesystem::permissions(dst, std::filesystem::perms::none);
    if (std::ifstream(dst, std::ios::binary).is_open()) {
        std::filesystem::permissions(dst, std::filesystem::perms::owner_all);
        skip("permissions do not deny reading here");
    }

    auto r = stage_file(src, dst, no_retry(Verify::Size));
    auto content = r.has_value() ? std::string() : r.error().message;
    std::filesystem::permissions(dst, std::filesystem::perms::owner_all);
    expect(fatal(r.has_value())) << content;
    expect(!r->copied);
    expect(eq(read_file(dst), "BBBB"));
}};

UTest different_size_is_always_copied{"BuildStage/DifferentSizeIsAlwaysCopied", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "longer-payload");
    write_file(dst, "short");

    auto r = stage_file(src, dst, no_retry());
    expect(fatal(r.has_value()));
    expect(bool(r->copied));
    expect(eq(read_file(dst), "longer-payload"));
}};

// Workspace design 2026-09-29 §5.3: a shared library placed in two product
// directories holds one copy of its bytes where the file system allows a link;
// any other file is copied, so a program that writes a file beside itself
// never writes through to its source.
UTest a_shared_library_is_placed_by_a_link_and_a_data_file_by_a_copy{"BuildStage/ASharedLibraryIsPlacedByALinkAndADataFileByACopy", [] {
    Tmp tmp;
    auto lib  = tmp.path / "store" / "libfoo.so.1";
    auto data = tmp.path / "store" / "table.dat";
    write_file(lib, "library");
    write_file(data, "data");
    for (auto const& dir : {"cli", "gui"}) {
        expect(fatal(bool(stage_file(lib, tmp.path / "bin" / dir / "libfoo.so.1", no_retry()))));
        expect(fatal(bool(stage_file(data, tmp.path / "bin" / dir / "table.dat", no_retry()))));
    }
    std::error_code ec;
    const auto libLinks = std::filesystem::hard_link_count(lib, ec);
    expect(fatal(!ec));
    // Three names on a file system with links (the source and two
    // placements), one where the link fell back to a copy.
    expect(libLinks == 3 || libLinks == 1) << libLinks;
    if (libLinks == 3)
        expect(bool(std::filesystem::equivalent(tmp.path / "bin" / "cli" / "libfoo.so.1",
                                                tmp.path / "bin" / "gui" / "libfoo.so.1")));
    expect(eq(std::filesystem::hard_link_count(data), 1u));
    expect(eq(read_file(tmp.path / "bin" / "gui" / "table.dat"), "data"));
}};

// A linked placement is replaced out of place: a new source never rewrites the
// bytes the earlier source shares with the placement.
UTest replacing_a_linked_placement_leaves_the_earlier_source_intact{"BuildStage/ReplacingALinkedPlacementLeavesTheEarlierSourceIntact", [] {
    Tmp tmp;
    auto first  = tmp.path / "v1" / "foo.dll";
    auto second = tmp.path / "v2" / "foo.dll";
    auto dst    = tmp.path / "bin" / "app" / "foo.dll";
    write_file(first, "version one");
    write_file(second, "version two!");
    expect(fatal(bool(stage_file(first, dst, no_retry()))));
    expect(fatal(bool(stage_file(second, dst, no_retry()))));
    expect(eq(read_file(dst), "version two!"));
    expect(eq(read_file(first), "version one"));
}};

UTest missing_source_is_an_error{"BuildStage/MissingSourceIsAnError", [] {
    Tmp tmp;
    auto r = stage_file(tmp.path / "nope.bin", tmp.path / "dst.bin", no_retry());
    expect(fatal(!r.has_value()));
    expect(ne(r.error().message.find("nope.bin"), std::string::npos));
}};

// A read-only destination that must actually be replaced: POSIX can do it
// (the directory entry is what gets rewritten, so temp-file + rename wins),
// Windows cannot (replacing a read-only file is denied however you spell it) —
// and there the contract is the loud failure, not a silent skip. Asserted per
// platform rather than skipped, so neither side can rot unnoticed.
UTest read_only_destination_outcome_is_platform_defined{"BuildStage/ReadOnlyDestinationOutcomeIsPlatformDefined", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "new-content");
    write_file(dst, "old");
    std::filesystem::permissions(dst, std::filesystem::perms::owner_read);

    auto r = stage_file(src, dst, no_retry());
#if defined(_WIN32)
    expect(fatal(!r.has_value()));
    expect(ne(r.error().message.find("help:"), std::string::npos));
    expect(eq(read_file(dst), "old"));
#else
    expect(fatal(r.has_value())) << r.error().message;
    expect(bool(r->copied));
    expect(eq(read_file(dst), "new-content"));
#endif
}};

// When staging genuinely cannot proceed, the failure must name the file and
// carry the actionable hint — never be downgraded to a warning.
UTest unwritable_destination_fails_with_actionable_hint{"BuildStage/UnwritableDestinationFailsWithActionableHint", [] {
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    write_file(src, "payload");
    // A directory in the destination's place: neither rename nor copy can win.
    auto dst = tmp.path / "occupied";
    std::filesystem::create_directories(dst / "child");
    write_file(dst / "child" / "keep.txt", "x");

    auto r = stage_file(src, dst, no_retry());
    expect(fatal(!r.has_value()));
    expect(ne(r.error().message.find("help:"), std::string::npos));
    expect(ne(r.error().message.find("clangd"), std::string::npos));
    expect(ne(r.error().message.find(dst.string()), std::string::npos));
}};

UTest same_content_compares_bytes_not_just_size{"BuildStage/SameContentComparesBytesNotJustSize", [] {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    write_file(a, std::string(300000, 'x'));
    write_file(b, std::string(300000, 'x'));
    write_file(c, std::string(300000, 'x').replace(299999, 1, "y"));

    expect(bool(same_content(a, b)));
    expect(!same_content(a, c));
    expect(!same_content(a, tmp.path / "missing.bin"));
}};

UTest verify_mode_parsing{"BuildStage/VerifyModeParsing", [] {
    expect(eq(parse_verify("content"), Verify::Content));
    expect(eq(parse_verify("size"), Verify::Size));
    // Anything unrecognized must land on the SAFE mode, not the fast one.
    expect(eq(parse_verify("nonsense"), Verify::Content));
    expect(eq(parse_verify(""), Verify::Content));
}};

// ── SPEC-007 R4.2 (mcpp#723): a destination with more than one source ──────

UTest one_source_is_identical_to_stage_file{"BuildStageFiles/OneSourceIsIdenticalToStageFile", [] {
    // The whole point of `stage_files`: a project with exactly one source per
    // destination -- every project before this feature -- must not notice a
    // difference. This is not asserted by string-matching a ninja line (that
    // is `test_ninja_backend.cpp`'s job); here it is the outcome that must
    // match `stage_file`'s exactly.
    Tmp tmp;
    auto src = tmp.path / "src.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(src, "payload");

    auto r = stage_files({src}, dst, no_retry());
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error().message;
    expect(bool(r->copied));
    expect(eq(read_file(dst), "payload"));
}};

UTest agreeing_sources_are_placed{"BuildStageFiles/AgreeingSourcesArePlaced", [] {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "shared bytes");
    write_file(b, "shared bytes");
    write_file(c, "shared bytes");

    auto r = stage_files({a, b, c}, dst, no_retry());
    if (auto&& failed = expect(fatal(r.has_value())); !failed) failed << r.error().message;
    expect(bool(r->copied));
    expect(eq(read_file(dst), "shared bytes"));
}};

UTest an_already_equivalent_destination_is_not_touched{"BuildStageFiles/AnAlreadyEquivalentDestinationIsNotTouched", [] {
    // The #311 property (BuildStage.EquivalentDestinationIsNotTouched) must
    // survive going through the multi-source entry point too.
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "same-bytes");
    write_file(b, "same-bytes");
    write_file(dst, "same-bytes");
    auto before = std::filesystem::file_time_type::clock::now() - std::chrono::hours{2};
    std::filesystem::last_write_time(dst, before);
    auto recorded = std::filesystem::last_write_time(dst);

    auto r = stage_files({a, b}, dst, no_retry());
    expect(fatal(r.has_value()));
    expect(!r->copied);
    expect(eq(std::filesystem::last_write_time(dst), recorded));
}};

UTest disagreeing_sources_fail_naming_every_one_and_the_destination{"BuildStageFiles/DisagreeingSourcesFailNamingEveryOneAndTheDestination", [] {
    Tmp tmp;
    auto a = tmp.path / "qtbase_zh_CN.qm";
    auto b = tmp.path / "other" / "qtbase_zh_CN.qm";
    auto dst = tmp.path / "bin" / "translations" / "qt_zh_CN.qm";
    write_file(a, "catalog A");
    write_file(b, "catalog B");

    auto r = stage_files({a, b}, dst, no_retry());
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().message.find(a.string()), std::string::npos)); !failed) failed << r.error().message;
    if (auto&& failed = expect(ne(r.error().message.find(b.string()), std::string::npos)); !failed) failed << r.error().message;
    if (auto&& failed = expect(ne(r.error().message.find(dst.string()), std::string::npos)); !failed) failed << r.error().message;
    // Refused, not silently resolved to either source: the destination is
    // untouched (no partial write from a rejected attempt).
    expect(!std::filesystem::exists(dst));
}};

UTest three_sources_where_only_the_last_disagrees_are_all_named{"BuildStageFiles/ThreeSourcesWhereOnlyTheLastDisagreesAreAllNamed", [] {
    // A pairwise comparison against only the first neighbour would miss a
    // mismatch between the second and third; every source must be compared.
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "b.bin";
    auto c = tmp.path / "c.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "X");
    write_file(b, "X");
    write_file(c, "Y");

    auto r = stage_files({a, b, c}, dst, no_retry());
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().message.find(c.string()), std::string::npos)); !failed) failed << r.error().message;
}};

UTest a_missing_source_is_named_as_missing_not_as_a_disagreement{"BuildStageFiles/AMissingSourceIsNamedAsMissingNotAsADisagreement", [] {
    Tmp tmp;
    auto a = tmp.path / "a.bin";
    auto b = tmp.path / "never-written.bin";
    auto dst = tmp.path / "dst.bin";
    write_file(a, "X");

    auto r = stage_files({a, b}, dst, no_retry());
    expect(fatal(!r.has_value()));
    if (auto&& failed = expect(ne(r.error().message.find("does not exist"), std::string::npos)); !failed) failed << r.error().message;
    if (auto&& failed = expect(ne(r.error().message.find(b.string()), std::string::npos)); !failed) failed << r.error().message;
    if (auto&& failed = expect(eq(r.error().message.find("disagree"), std::string::npos)); !failed) failed << r.error().message;
    expect(!std::filesystem::exists(dst));
}};

UTest empty_source_list_is_an_error{"BuildStageFiles/EmptySourceListIsAnError", [] {
    Tmp tmp;
    auto r = stage_files({}, tmp.path / "dst.bin", no_retry());
    expect(fatal(!r.has_value()));
}};
