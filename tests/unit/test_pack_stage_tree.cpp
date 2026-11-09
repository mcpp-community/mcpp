
import std;
import mcpplibs.mtest;
import mcpp.pack.stage_tree;

using namespace mcpplibs::mtest;

// The staged tree is what `mcpp pack` computes and, until `${mcpp.stage_dir}`,
// then threw away. Two things about it are the engine's contract with a
// distribution member, and both are asserted here rather than end to end,
// because the end-to-end criterion for either is "the distributable is
// rebuilt", which a wrong answer also satisfies.

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_stage_tree_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
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

} // namespace

UTest the_manifest_is_a_sibling_and_never_a_member{"PackStageTree/TheManifestIsASiblingAndNeverAMember", [] {
    Tmp t;
    auto stage = t.path / "app-1.0.0-x86_64-linux-gnu";
    std::filesystem::create_directories(stage);

    auto manifest = mcpp::pack::stage_manifest_path(stage);
    expect(eq(manifest.parent_path(), stage.parent_path()));
    // A file INSIDE the tree would be collected by every format that packages
    // the directory wholesale, and would then ship inside the user's
    // installer. Asserted as a path relationship because that is the property,
    // and a spelling change that broke it would otherwise only show up as an
    // extra file in a released package.
    expect(!(manifest.string().starts_with(stage.string() + "/")));
    expect(!(manifest.string().starts_with(stage.string() + "\\")));

    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    std::error_code ec;
    for (auto const& e : std::filesystem::recursive_directory_iterator(stage, ec))
        fail([&] { std::ostringstream text; text << "the manifest landed inside the tree: " << e.path().string(); return text.str(); }());
}};

UTest the_manifest_changes_when_the_staged_set_does{"PackStageTree/TheManifestChangesWhenTheStagedSetDoes", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "0123456789");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    const auto first = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(ne(first.find("bin/app"), std::string::npos));
    expect(ne(first.find("10 "), std::string::npos));

    // A DEPENDENCY'S SHARED LIBRARY JOINING THE CLOSURE. This is the case the
    // manifest exists for: the program's own bytes need not have changed, so an
    // edge that depended only on the link output would report the previous
    // distributable as up to date.
    write_file(stage / "lib" / "libdep.so.1", "xx");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    const auto second = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(ne(first, second));
    expect(ne(second.find("lib/libdep.so.1"), std::string::npos));

    // A staged file whose LENGTH changed, with no entry added or removed.
    write_file(stage / "bin" / "app", "0123456789abcdef");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    expect(ne(read_file(mcpp::pack::stage_manifest_path(stage)), second));
}};

UTest staging_the_same_tree_twice_leaves_the_manifest_untouched{"PackStageTree/StagingTheSameTreeTwiceLeavesTheManifestUntouched", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "same");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    auto manifest = mcpp::pack::stage_manifest_path(stage);
    const auto before = std::filesystem::last_write_time(manifest);

    // Rewriting identical bytes would move the mtime, and a moved mtime on an
    // input is indistinguishable from a changed input: a pack that staged the
    // same tree twice would rebuild the distributable both times. Same rule
    // `mcpp.build.stage` states at length for a staged BMI.
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    expect(eq(std::filesystem::last_write_time(manifest), before));
}};

UTest the_order_of_a_directory_walk_is_not_a_promise{"PackStageTree/TheOrderOfADirectoryWalkIsNotAPromise", [] {
    // Two trees with the same contents produce the same bytes. Without the
    // sort, an iteration order that differed between runs would make the dist
    // edge dirty on every pack for no reason -- which reads as "packaging is
    // slow" rather than as a defect.
    Tmp a, b;
    for (auto const& root : {a.path, b.path}) {
        write_file(root / "s" / "bin" / "app", "aa");
        write_file(root / "s" / "lib" / "z.so", "bbb");
        write_file(root / "s" / "share" / "doc" / "readme", "c");
        expect(fatal(bool(mcpp::pack::write_stage_manifest(root / "s"))));
    }
    expect(eq(read_file(mcpp::pack::stage_manifest_path(a.path / "s")), read_file(mcpp::pack::stage_manifest_path(b.path / "s"))));
}};

UTest the_engine_owns_five_format_names{"PackStageTree/TheEngineOwnsFiveFormatNames", [] {
    // `tar`, `zip`, `dir`, `single-file` and `polyglot` are the shapes `mcpp pack` owns
    // (SPEC-014 §1.3); every other value of `--pkg-format` is a name a
    // package provides. The list lives beside the staged tree because two
    // layers need the same answer -- the listing and the refusal of an
    // unknown name, and the declaration check, which warns of a package that
    // claims one of them and would therefore be silently unreachable.
    for (auto name : { "tar", "zip", "dir", "single-file", "polyglot" })
        expect(mcpp::pack::is_builtin_pack_format(name)) << name;
    expect(!mcpp::pack::is_builtin_pack_format("appimage"));
    expect(!mcpp::pack::is_builtin_pack_format("okb"));
    expect(!mcpp::pack::is_builtin_pack_format("msi"));
    expect(!mcpp::pack::is_builtin_pack_format(""));
    expect(eq(mcpp::pack::kBuiltinPackFormats.size(), 5u));
}};

// ── #630 §3: the `closure` field ─────────────────────────────────────────
//
// The "not-walked" case (a Mach-O program, or a non-PE artifact on a Windows
// host) cannot be produced on this (Linux) runner through `pack::run` -- it
// is covered end to end on macOS/iOS CI instead (see the e2e fixture's
// comment). What CAN be tested here, on every host, is the writer/reader
// contract itself: both values round-trip through the manifest file.

UTest default_closure_is_walked_and_says_so{"PackStageTree/DefaultClosureIsWalkedAndSaysSo", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "x");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage))));
    auto text = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(eq(text.substr(0, text.find('\n')), "closure = walked"));
    expect(eq(text.find("reason ="), std::string::npos));
}};

UTest not_walked_records_the_reason_on_one_line{"PackStageTree/NotWalkedRecordsTheReasonOnOneLine", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "x");
    mcpp::pack::ClosureStatus closure{
        .walked = false,
        .reason = "cannot package the Mach-O program 'app' yet.\n"
                  "       The dependency closure for that format is resolved "
                  "by running the artifact under\n"
                  "       the target's own dynamic linker.",
    };
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage, closure))));
    auto text = read_file(mcpp::pack::stage_manifest_path(stage));
    auto firstLine = text.substr(0, text.find('\n'));
    expect(eq(firstLine, "closure = not-walked"));
    // Folded to ONE line -- the manifest is one entry per line, and the
    // reason text carries its own embedded newlines because it doubles as a
    // CLI diagnostic.
    auto secondLineEnd = text.find('\n', text.find('\n') + 1);
    auto secondLine = text.substr(text.find('\n') + 1, secondLineEnd - text.find('\n') - 1);
    expect(secondLine.starts_with("reason = cannot package the Mach-O program"));
    expect(eq(secondLine.find('\n'), std::string::npos));
    // The rest of the manifest -- the staged file listing -- is unaffected.
    expect(ne(text.find("bin/app"), std::string::npos));
}};

UTest walked_and_not_walked_produce_different_manifests{"PackStageTree/WalkedAndNotWalkedProduceDifferentManifests", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "x");
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage, mcpp::pack::ClosureStatus{true, ""}))));
    auto walked = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(fatal(bool(mcpp::pack::write_stage_manifest(
        stage, mcpp::pack::ClosureStatus{false, "no such linker to ask"}))));
    auto notWalked = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(ne(walked, notWalked));
}};

// ── #634 A3: the `needs` lines ──────────────────────────────────────────

UTest needs_lines_follow_the_header_sorted_and_once_each{"PackStageTree/NeedsLinesFollowTheHeaderSortedAndOnceEach", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "lib" / "libfw.so", "fw");
    using K = mcpp::pack::ClosureNeed::Kind;
    mcpp::pack::ClosureStatus closure;
    // Out of order, and one platform name twice -- a several-ABI tree reads
    // the same one once per leg.
    closure.needs = {
        {"libfw.so", K::Staged,   "lib/libfw.so"},
        {"libc.so",  K::Platform, {}},
        {"libc.so",  K::Platform, {}},
    };
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage, closure))));
    auto text = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(eq(text, "closure = walked\n"
        "needs\tlibc.so\tplatform\n"
        "needs\tlibfw.so\tlib/libfw.so\n"
        "2 lib/libfw.so\n"));
}};

UTest an_incomplete_closure_lists_its_unresolved_names_after_the_reason{"PackStageTree/AnIncompleteClosureListsItsUnresolvedNamesAfterTheReason", [] {
    Tmp t;
    auto stage = t.path / "app";
    write_file(stage / "bin" / "app", "x");
    using K = mcpp::pack::ClosureNeed::Kind;
    mcpp::pack::ClosureStatus closure;
    closure.walked = false;
    closure.reason = "the dependency closure of 'app' is incomplete";
    closure.needs  = {{"@rpath/libgone.dylib", K::Unresolved, {}}};
    expect(fatal(bool(mcpp::pack::write_stage_manifest(stage, closure))));
    auto text = read_file(mcpp::pack::stage_manifest_path(stage));
    expect(eq(text, "closure = not-walked\n"
        "reason = the dependency closure of 'app' is incomplete\n"
        "needs\t@rpath/libgone.dylib\tunresolved\n"
        "1 bin/app\n"));
}};

UTest a_name_with_a_space_stays_one_field{"PackStageTree/ANameWithASpaceStaysOneField", [] {
    using K = mcpp::pack::ClosureNeed::Kind;
    expect(eq(mcpp::pack::render_closure_need(
                  {"@rpath/My Kit.framework/My Kit", K::Staged, "bin/My Kit"}), "needs\t@rpath/My Kit.framework/My Kit\tbin/My Kit"));
}};

UTest a_missing_tree_is_refused_rather_than_described_as_empty{"PackStageTree/AMissingTreeIsRefusedRatherThanDescribedAsEmpty", [] {
    Tmp t;
    // An empty manifest for a directory that does not exist would say "nothing
    // is staged", which is what a correct pack of an empty bundle also says.
    // The two must not be spelled alike.
    expect(!mcpp::pack::write_stage_manifest(t.path / "never-staged"));
    expect(!(std::filesystem::exists(
        mcpp::pack::stage_manifest_path(t.path / "never-staged"))));
}};

// ── #622 A5: the wasm32-emscripten stem family ───────────────────────────
// "The launcher is the executable; the family is what the link also wrote" —
// asked of the directory, never of a fixed extension list.

UTest the_required_wasm_module_is_found{"PackStageTree/TheRequiredWasmModuleIsFound", [] {
    Tmp t;
    write_file(t.path / "app.js", "launcher");
    write_file(t.path / "app.wasm", "\0asm");
    auto fam = mcpp::pack::emscripten_stem_family(t.path, "app.js");
    expect(bool(fam.hasWasm));
    expect(eq(fam.siblings, (std::vector<std::string>{"app.wasm"})));
}};

// The negative direction: a launcher with no `.wasm` beside it (a build
// directory that does not match the graph) must report the absence, not a
// family that happens to be empty of the required member.
UTest a_missing_wasm_module_is_reported{"PackStageTree/AMissingWasmModuleIsReported", [] {
    Tmp t;
    write_file(t.path / "app.js", "launcher");
    auto fam = mcpp::pack::emscripten_stem_family(t.path, "app.js");
    expect(!fam.hasWasm);
    expect(fam.siblings.empty());
}};

// Optional siblings travel when the link wrote them, and only files that
// actually share the stem are picked up — a neighbouring, unrelated launcher
// in the same directory (a second target) must not bleed into this one's
// family.
UTest optional_siblings_travel_and_unrelated_files_do_not{"PackStageTree/OptionalSiblingsTravelAndUnrelatedFilesDoNot", [] {
    Tmp t;
    write_file(t.path / "app.js", "launcher");
    write_file(t.path / "app.wasm", "\0asm");
    write_file(t.path / "app.data", "preloaded");
    write_file(t.path / "app.worker.js", "worker");
    write_file(t.path / "other.js", "a different target's launcher");
    write_file(t.path / "other.wasm", "\0asm");
    auto fam = mcpp::pack::emscripten_stem_family(t.path, "app.js");
    expect(bool(fam.hasWasm));
    expect(eq(fam.siblings, (std::vector<std::string>{
        "app.data", "app.wasm", "app.worker.js"})));
}};
