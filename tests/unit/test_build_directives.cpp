
import std;
import mcpplibs.mtest;
import mcpp.build.directives;
import mcpp.manifest;
import mcpp.toolchain.dialect;

using namespace mcpplibs::mtest;

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

UTest every_row_is_internally_consistent{"BuildDirectives/EveryRowIsInternallyConsistent", [] {
    for (auto const& def : dirs::kTable) {
        expect(!def.wire.empty());
        expect(gt(def.sinceProtocol, 0)) << def.wire;
        expect(le(def.sinceProtocol, dirs::kProtocolVersion)) << def.wire;
        // A RerunKey directive feeds only the re-run key, so it must NOT be
        // persisted as a `d` record; everything else must be, or a cache hit
        // would silently apply less than the program asked for.
        if (def.scope == dirs::Scope::RerunKey)
            expect(def.tag.empty()) << def.wire;
        else
            expect(!def.tag.empty()) << def.wire;
        // The declared-output contract needs a diagnostic that says WHICH
        // contract was broken.
        if (def.mustExistAfterRun) {
            expect(!def.missingPrefix.empty()) << def.wire;
            expect(!def.missingSuffix.empty()) << def.wire;
        }
    }
}};

UTest wire_names_are_unique{"BuildDirectives/WireNamesAreUnique", [] {
    std::set<std::string_view> seen;
    for (auto const& def : dirs::kTable)
        expect(bool(seen.insert(def.wire).second)) << "duplicate wire " << def.wire;
}};

UTest rows_sharing_a_tag_share_a_slot{"BuildDirectives/RowsSharingATagShareASlot", [] {
    // Cache deserialization resolves a tag to exactly one slot, so two rows
    // with the same tag but different slots would round-trip to the wrong
    // channel. link-lib and link-search legitimately share `ldflag`.
    std::map<std::string_view, dirs::Slot> slotOfTag;
    for (auto const& def : dirs::kTable) {
        if (def.tag.empty()) continue;
        auto [it, fresh] = slotOfTag.try_emplace(def.tag, def.slot);
        if (!fresh) expect(eq(it->second, def.slot)) << def.tag;
    }
}};

UTest lookups_agree{"BuildDirectives/LookupsAgree", [] {
    for (auto const& def : dirs::kTable) {
        expect(fatal(ne(dirs::find_by_wire(def.wire), nullptr))) << def.wire;
        expect(eq(dirs::find_by_wire(def.wire)->slot, def.slot)) << def.wire;
        if (!def.tag.empty()) {
            expect(fatal(ne(dirs::find_by_tag(def.tag), nullptr))) << def.tag;
            expect(eq(dirs::find_by_tag(def.tag)->slot, def.slot)) << def.tag;
        }
    }
    expect(eq(dirs::find_by_wire("no-such-directive"), nullptr));
    // The empty tag belongs to the rerun rows and must never resolve, or a
    // malformed cache line would be silently accepted into a slot.
    expect(eq(dirs::find_by_tag(""), nullptr));
}};

// ── Parsing ────────────────────────────────────────────────────────────────

UTest non_directive_lines_are_ignored{"BuildDirectives/NonDirectiveLinesAreIgnored", [] {
    auto d = parse("hello\nmcpp is not a directive\n  \n");
    for (std::size_t i = 0; i < dirs::kSlotCount; ++i)
        expect(d.slots[i].empty());
    expect(d.unknownKeys.empty());
    expect(eq(d.protocol, 0));
}};

UTest transforms_are_applied_once_at_parse_time{"BuildDirectives/TransformsAreAppliedOnceAtParseTime", [] {
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
    expect(eq(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-Wall"})));
    // link-lib and link-search share the ldflags slot, in emission order.
    expect(eq(d.at(dirs::Slot::LdFlags), (std::vector<std::string>{"-lz", "-L" + under_root("vendor/lib")})));
    expect(eq(d.at(dirs::Slot::Defines), (std::vector<std::string>{"-DHAVE_X"})));
    // Relative resolves against the package root; absolute is taken as-is.
    expect(eq(d.at(dirs::Slot::IncludeDirs), (std::vector<std::string>{under_root("inc")})));
    expect(eq(d.at(dirs::Slot::IncludeDirsAfter), (std::vector<std::string>{alreadyAbs.string()})));
}};

UTest dialect_decides_the_spelling{"BuildDirectives/DialectDecidesTheSpelling", [] {
    dirs::Directives d;
    dirs::accept_output(d, mcpp::toolchain::msvc_dialect(), test_root(),
                        "mcpp:link-lib=z\nmcpp:cfg=HAVE_X\n");
    expect(eq(d.at(dirs::Slot::LdFlags), (std::vector<std::string>{"z.lib"})));
    expect(eq(d.at(dirs::Slot::Defines), (std::vector<std::string>{"/DHAVE_X"})));
}};

UTest values_may_contain_spaces_and_equals_signs{"BuildDirectives/ValuesMayContainSpacesAndEqualsSigns", [] {
    // Everything after the FIRST '=' is the value — a path with a space or a
    // define with an '=' must survive intact.
    auto d = parse("mcpp:cxxflag=-DMSG=\"a b\"\n");
    expect(eq(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-DMSG=\"a b\""})));
}};

// ── Protocol ───────────────────────────────────────────────────────────────

UTest unknown_key_without_an_announcement_is_tolerated{"BuildDirectives/UnknownKeyWithoutAnAnnouncementIsTolerated", [] {
    // A hand-written printf program announces nothing; its surface is frozen,
    // so an unknown key is a typo the engine warns about rather than a
    // forward-compat situation it must refuse.
    auto d = parse("mcpp:no-such-thing=1\n");
    expect(eq(d.protocol, 0));
    expect(eq(d.unknownKeys, (std::vector<std::string>{"no-such-thing"})));
    expect(!(dirs::protocol_error(d).has_value()));
}};

UTest unknown_key_with_an_announcement_is_fatal{"BuildDirectives/UnknownKeyWithAnAnnouncementIsFatal", [] {
    auto d = parse("mcpp:protocol=1\nmcpp:no-such-thing=1\n");
    expect(eq(d.protocol, 1));
    auto err = dirs::protocol_error(d);
    expect(fatal(err.has_value()));
    expect(ne(err->find("no-such-thing"), std::string::npos));
}};

UTest newer_protocol_is_fatal_and_says_what_to_do{"BuildDirectives/NewerProtocolIsFatalAndSaysWhatToDo", [] {
    auto d = parse("mcpp:protocol=999\n");
    auto err = dirs::protocol_error(d);
    expect(fatal(err.has_value()));
    expect(ne(err->find("999"), std::string::npos));
    // The message has to tell the user the actionable thing, not just that
    // something is wrong.
    expect(ne(err->find("upgrade"), std::string::npos));
}};

UTest current_protocol_is_accepted{"BuildDirectives/CurrentProtocolIsAccepted", [] {
    auto d = parse(std::format("mcpp:protocol={}\nmcpp:cxxflag=-Wall\n",
                               dirs::kProtocolVersion));
    expect(!(dirs::protocol_error(d).has_value()));
    expect(eq(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-Wall"})));
}};

UTest unknown_keys_are_deduplicated{"BuildDirectives/UnknownKeysAreDeduplicated", [] {
    auto d = parse("mcpp:zzz=1\nmcpp:zzz=2\nmcpp:yyy=3\n");
    expect(eq(d.unknownKeys, (std::vector<std::string>{"zzz", "yyy"})));
}};

// #693: a directive whose text is not UTF-8 names a different file once it is
// in build.ninja, so it is refused by key, whatever protocol the program
// speaks, and nothing of it is applied.
UTest a_directive_that_is_not_utf8_is_refused_by_key{"BuildDirectives/ADirectiveThatIsNotUtf8IsRefusedByKey", [] {
    // "caf" + 0xE9: Latin-1, the shape a narrow program in a code page 1252
    // process prints.
    auto d = parse("mcpp:include-dir=inc/caf\xE9\nmcpp:cxxflag=-Wall\n");
    expect(d.at(dirs::Slot::IncludeDirs).empty());
    expect(eq(d.at(dirs::Slot::CxxFlags), (std::vector<std::string>{"-Wall"})));
    expect(eq(d.nonUtf8Keys, (std::vector<std::string>{"mcpp:include-dir"})));
    auto err = dirs::encoding_error(d);
    expect(fatal(err.has_value()));
    if (auto&& failed = expect(ne(err->find("mcpp:include-dir"), std::string::npos)); !failed) failed << *err;
    if (auto&& failed = expect(ne(err->find("UTF-8"), std::string::npos)); !failed) failed << *err;
}};

UTest utf_eight_directives_and_other_chatter_pass{"BuildDirectives/UtfEightDirectivesAndOtherChatterPass", [] {
    // U+00E9 in UTF-8 is accepted; a line that is not a directive is not
    // examined at all.
    auto d = parse("mcpp:include-dir=inc/caf\xC3\xA9\nnoise \xE9\n");
    expect(eq(d.at(dirs::Slot::IncludeDirs).size(), 1u));
    expect(d.nonUtf8Keys.empty());
    expect(!(dirs::encoding_error(d).has_value()));
}};

// ── Cache round-trip ───────────────────────────────────────────────────────

UTest serialize_deserialize_round_trip{"BuildDirectives/SerializeDeserializeRoundTrip", [] {
    auto d = parse("mcpp:cxxflag=-Wall\n"
                   "mcpp:cflag=-std=c11\n"
                   "mcpp:link-lib=z\n"
                   "mcpp:link-search=vendor/lib\n"
                   "mcpp:cfg=HAVE_X\n"
                   "mcpp:generated=src/gen.cpp\n"
                   "mcpp:source=vendor/pick.cpp\n"
                   "mcpp:include-dir=inc\n"
                   "mcpp:include-dir-after=after\n"
                   "mcpp:fact=widget.driver=1.2\n"
                   "mcpp:floor=widget.driver >= 1.0\n"
                   "mcpp:pack-format=appimage\n"
                   "mcpp:windows-subsystem=gui:windows\n"
                   "mcpp:windows-entry=gui:wWinMain\n");

    std::ostringstream os;
    dirs::serialize(os, d);

    dirs::Directives back;
    std::istringstream is(os.str());
    std::string line;
    while (std::getline(is, line)) {
        expect(fatal(line.starts_with("d ")));
        auto rest = line.substr(2);
        auto sp = rest.find(' ');
        expect(fatal(ne(sp, std::string::npos)));
        expect(bool(dirs::accept_cache_record(back, rest.substr(0, sp),
                                              rest.substr(sp + 1))));
    }

    // Every persisted slot survives verbatim; the rerun slots deliberately do
    // not (they are re-derived from their own cache records).
    for (auto const& def : dirs::kTable) {
        if (def.tag.empty()) continue;
        expect(eq(back.at(def.slot), d.at(def.slot))) << def.wire;
    }
}};

// ── The probe channel ──────────────────────────────────────────────────────
//
// A rule package measures, the engine compares. What arrives on the wire is
// folded into the SAME runtime declarations a manifest could have written,
// so the version-floor check in prepare reads one list and never learns
// which spelling a claim came in.

UTest facts_and_floors_are_claims_that_fold_into_runtime_declarations{"BuildDirectives/FactsAndFloorsAreClaimsThatFoldIntoRuntimeDeclarations", [] {
    auto d = parse("mcpp:fact=widget.driver=1.2\n"
                   "mcpp:floor=widget.driver >= 2.0\n");
    expect(eq(d.at(dirs::Slot::Facts), (std::vector<std::string>{"widget.driver=1.2"})));
    expect(eq(d.at(dirs::Slot::Floors), (std::vector<std::string>{"widget.driver >= 2.0"})));

    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.provides.size(), 1u)));
    expect(eq(m.runtimeConfig.provides[0], "widget.driver=1.2"));
    expect(fatal(eq(m.runtimeConfig.requirements.size(), 1u)));
    expect(eq(m.runtimeConfig.requirements[0].kind, "version-floor"));
    expect(eq(m.runtimeConfig.requirements[0].value, "widget.driver >= 2.0"));
    // A claim about the build, not about the link or the run: the phase says
    // when it is decided.
    expect(eq(m.runtimeConfig.requirements[0].phase, "build"));
}};

UTest a_pack_format_is_a_name_carried_and_not_interpreted{"BuildDirectives/APackFormatIsANameCarriedAndNotInterpreted", [] {
    auto d = parse("mcpp:pack-format=appimage\n"
                   "mcpp:pack-format=msi\n");
    expect(eq(d.at(dirs::Slot::PackFormats), (std::vector<std::string>{"appimage", "msi"})));

    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(eq(m.buildConfig.packFormats, (std::vector<std::string>{"appimage", "msi"})));
    // The engine holds the DISPATCH and no format, so a name reaches neither
    // the compile line nor the link line. A row that leaked into either would
    // put dpkg's or WiX's vocabulary on a command line.
    expect(m.buildConfig.cflags.empty());
    expect(m.buildConfig.cxxflags.empty());
    expect(m.buildConfig.ldflags.empty());
    expect(m.buildConfig.sources.empty());
    expect(m.buildConfig.actions.empty());
}};

UTest a_pack_format_declaration_is_persisted{"BuildDirectives/APackFormatDeclarationIsPersisted", [] {
    // THE HALF THAT WOULD OTHERWISE BE MISSED. A build program's result is
    // cached and a hit does not re-run it, and the pass that READS this set is
    // `mcpp pack`, which is never a project's first build. A declaration that
    // was not persisted would therefore be present exactly once and absent
    // every time it mattered, and `--format <name>` would refuse naming
    // nothing.
    //
    // `SerializeDeserializeRoundTrip` above asserts the round trip for every
    // tagged row, so this only has to hold the field that puts the row in that
    // set -- which is the field a new row is most likely to be added without.
    const auto* def = dirs::find_by_wire("pack-format");
    expect(fatal(ne(def, nullptr)));
    expect(!(def->tag.empty())) << "a tag-less row is not replayed on a cache hit";
    expect(eq(def->slot, dirs::Slot::PackFormats));
    expect(eq(def->scope, dirs::Scope::Claim));
}};

UTest a_claim_reaches_neither_compile_nor_link{"BuildDirectives/AClaimReachesNeitherCompileNorLink", [] {
    auto d = parse("mcpp:fact=widget.driver=1.2\n"
                   "mcpp:floor=widget.driver >= 2.0\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(m.buildConfig.cflags.empty());
    expect(m.buildConfig.cxxflags.empty());
    expect(m.buildConfig.ldflags.empty());
    expect(m.buildConfig.sources.empty());
    for (auto const& def : dirs::kTable)
        if (def.slot == dirs::Slot::Facts || def.slot == dirs::Slot::Floors)
            expect(eq(def.scope, dirs::Scope::Claim)) << def.wire;
}};

UTest rerun_slots_are_not_persisted_as_directives{"BuildDirectives/RerunSlotsAreNotPersistedAsDirectives", [] {
    auto d = parse("mcpp:rerun-if-changed=config.h\n"
                   "mcpp:rerun-if-env-changed=USE_FAST\n");
    expect(eq(d.at(dirs::Slot::RerunFiles), (std::vector<std::string>{"config.h"})));
    expect(eq(d.at(dirs::Slot::RerunEnv), (std::vector<std::string>{"USE_FAST"})));
    std::ostringstream os;
    dirs::serialize(os, d);
    expect(os.str().empty());
}};

UTest unknown_cache_tag_is_rejected_rather_than_dropped{"BuildDirectives/UnknownCacheTagIsRejectedRatherThanDropped", [] {
    // A cache written by a NEWER mcpp carries tags this one does not know.
    // Silently skipping them would apply a strict subset of what the program
    // asked for; the caller turns `false` into "entry is stale".
    dirs::Directives d;
    expect(!dirs::accept_cache_record(d, "some-future-tag", "value"));
    expect(bool(dirs::accept_cache_record(d, "cxxflag", "-Wall")));
}};

// ── Apply ──────────────────────────────────────────────────────────────────

UTest apply_routes_each_slot_to_its_manifest_channel{"BuildDirectives/ApplyRoutesEachSlotToItsManifestChannel", [] {
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

    expect(ne(std::find(bc.cxxflags.begin(), bc.cxxflags.end(), "-Wall"), bc.cxxflags.end()));
    expect(ne(std::find(bc.cflags.begin(), bc.cflags.end(), "-std=c11"), bc.cflags.end()));
    expect(ne(std::find(bc.ldflags.begin(), bc.ldflags.end(), "-lz"), bc.ldflags.end()));
    // A cfg define colours BOTH language channels — the one slot that fans out.
    expect(ne(std::find(bc.cflags.begin(), bc.cflags.end(), "-DHAVE_X"), bc.cflags.end()));
    expect(ne(std::find(bc.cxxflags.begin(), bc.cxxflags.end(), "-DHAVE_X"), bc.cxxflags.end()));
    // generated= and source= must reach BOTH source lists: the scanner walks
    // the legacy modules.sources mirror, and a file missing from it is
    // invisible to the module scan.
    for (auto const& s : {"src/gen.cpp", "vendor/pick.cpp"}) {
        expect(ne(std::find(bc.sources.begin(), bc.sources.end(), s), bc.sources.end())) << s;
        expect(ne(std::find(m.modules.sources.begin(), m.modules.sources.end(), s), m.modules.sources.end())) << s;
    }
    expect(ne(std::find(bc.includeDirs.begin(), bc.includeDirs.end(),
                        std::filesystem::path(under_root("inc"))), bc.includeDirs.end()));
    expect(ne(std::find(bc.includeDirsAfter.begin(), bc.includeDirsAfter.end(),
                        std::filesystem::path(under_root("after"))), bc.includeDirsAfter.end()));
}};

UTest apply_appends_rather_than_replaces{"BuildDirectives/ApplyAppendsRatherThanReplaces", [] {
    mcpp::manifest::Manifest m;
    m.buildConfig.cxxflags.push_back("-O2");
    dirs::apply(m, parse("mcpp:cxxflag=-Wall\n"));
    expect(eq(m.buildConfig.cxxflags, (std::vector<std::string>{"-O2", "-Wall"})));
}};

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

UTest fold_moves_only_the_tail_and_only_private_channels{"BuildDirectives/FoldMovesOnlyTheTailAndOnlyPrivateChannels", [] {
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
    expect(eq(priv.cxxflags, (std::vector<std::string>{"-Wall", "-DHAVE_X"})));
    expect(eq(priv.cflags, (std::vector<std::string>{"-DHAVE_X"})));
    expect(eq(priv.includeDirs, (std::vector<std::filesystem::path>{under_root("inc")})));
    // Link flags are NOT private — they reach the final link through their own
    // path, and folding them here would double-apply them.
    expect(priv.includeDirsAfter.empty());
}};

UTest fold_is_idempotent_on_include_dirs{"BuildDirectives/FoldIsIdempotentOnIncludeDirs", [] {
    // Include dirs are unique-appended: the same dir emitted twice, or a fold
    // replayed, must not grow the list.
    mcpp::manifest::Manifest m;
    auto before = dirs::mark(m);
    dirs::apply(m, parse("mcpp:include-dir=inc\nmcpp:include-dir=inc\n"));
    FakeUsage priv;
    dirs::fold_private_tail(priv, m, before);
    dirs::fold_private_tail(priv, m, before);
    expect(eq(priv.includeDirs, (std::vector<std::filesystem::path>{under_root("inc")})));
}};

// ── Run bound ──────────────────────────────────────────────────────────────

// T-3. The precedence is a PURE function of two optionals, which is the whole
// reason `run_timeout` was split from `env_timeout_override`: this table can
// be walked without touching the process environment.
//
// Nine rows, and the two that matter most are the `nullopt` vs `0` pairs.
// With a plain `int` those two would be the same value, and since 0 MEANS "no
// limit", every project that never mentions the key would silently lose its
// bound. That is why both levels are optionals all the way down.
UTest run_timeout_precedence_is_env_then_manifest_then_default{"BuildDirectives/RunTimeoutPrecedenceIsEnvThenManifestThenDefault", [] {
    using ms = std::chrono::milliseconds;
    constexpr auto def = ms(dirs::kDefaultRunTimeoutSecs * 1000);
    const std::optional<int> unset;

    // env, manifest, expected
    struct Row { std::optional<int> env, manifest; ms want; };
    const Row rows[] = {
        { unset, unset, def       },   // neither: the built-in bound
        { unset, 1800,  ms(1800'000) },// manifest alone
        { unset, 0,     ms(0)     },   // manifest asks for no bound
        { 30,    unset, ms(30'000)},   // env alone
        { 30,    1800,  ms(30'000)},   // env WINS over the manifest
        { 0,     1800,  ms(0)     },   // env 0 wins too — "no bound, now"
        { 30,    0,     ms(30'000)},   // ... and in the other direction
        { 0,     unset, ms(0)     },
        { 1,     1,     ms(1'000) },
    };
    for (auto const& r : rows) {
        if (auto&& failed = expect(eq(dirs::run_timeout(r.env, r.manifest), r.want)); !failed) failed << "env=" << (r.env ? std::to_string(*r.env) : "unset")
            << " manifest=" << (r.manifest ? std::to_string(*r.manifest) : "unset");
    }

    // Default: bounded. An unbounded build program is how a build hangs with
    // no diagnostic at all.
    expect(gt(dirs::run_timeout(unset, unset).count(), 0));
}};

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

UTest glob_fingerprint_changes_when_a_file_appears_or_disappears{"BuildDirectives/GlobFingerprintChangesWhenAFileAppearsOrDisappears", [] {
    GlobTree t{"mcpp_glob_fp_membership"};
    t.write("proto/a.proto", "syntax=\"proto3\";");
    auto one = t.fp("proto/**/*.proto");

    t.write("proto/b.proto", "syntax=\"proto3\";");
    auto two = t.fp("proto/**/*.proto");
    expect(ne(one, two));

    std::filesystem::remove(t.root / "proto/b.proto");
    expect(eq(t.fp("proto/**/*.proto"), one));
}};

UTest glob_fingerprint_ignores_content_size_and_timestamp{"BuildDirectives/GlobFingerprintIgnoresContentSizeAndTimestamp", [] {
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
    expect(eq(t.fp("proto/**/*.proto"), before));
}};

UTest glob_fingerprint_never_walks_the_build_output_tree{"BuildDirectives/GlobFingerprintNeverWalksTheBuildOutputTree", [] {
    // A build program writes its outputs INSIDE the project. If a wide pattern
    // included them the set would change on every build and the program would
    // re-run forever — the classic Cargo footgun. Enforced by the engine
    // rather than left to the author's pattern.
    GlobTree t{"mcpp_glob_fp_outdir"};
    t.write("proto/a.proto", "x");
    auto before = t.fp("**");
    t.write("target/.build-mcpp/out/a.pb.cc", "generated");
    t.write("target/.build-mcpp/out/a.pb.h", "generated");
    expect(eq(t.fp("**"), before));

    // .git is excluded for the same reason: it changes on every commit and
    // never means the build program's inputs changed.
    t.write(".git/HEAD", "ref: refs/heads/main");
    expect(eq(t.fp("**"), before));
}};

UTest glob_fingerprint_is_independent_of_directory_iteration_order{"BuildDirectives/GlobFingerprintIsIndependentOfDirectoryIterationOrder", [] {
    // The set is sorted before hashing, so two trees with the same members
    // agree regardless of the order the platform hands them back.
    GlobTree a{"mcpp_glob_fp_order_a"};
    GlobTree b{"mcpp_glob_fp_order_b"};
    for (auto n : { "z.proto", "a.proto", "m.proto" }) a.write(std::string("proto/") + n, "x");
    for (auto n : { "a.proto", "m.proto", "z.proto" }) b.write(std::string("proto/") + n, "y");
    expect(eq(a.fp("proto/**/*.proto"), b.fp("proto/**/*.proto")));
}};

UTest glob_directive_parses_into_its_own_slot{"BuildDirectives/GlobDirectiveParsesIntoItsOwnSlot", [] {
    auto d = parse("mcpp:rerun-if-changed-glob=proto/**/*.proto\n");
    expect(fatal(eq(d.at(dirs::Slot::RerunGlobs).size(), 1u)));
    expect(eq(d.at(dirs::Slot::RerunGlobs)[0], "proto/**/*.proto"));
    // A re-run key is not a build input, so it must not be persisted as a `d`
    // record — otherwise a cache hit would replay it as one.
    std::ostringstream os;
    dirs::serialize(os, d);
    expect(eq(os.str().find("proto/**"), std::string::npos)) << os.str();
}};

// ── link-script ────────────────────────────────────────────────────────────
//
// The row exists so a board-support package can hand a consumer the board's
// memory layout. Everything else that could carry it is either package-private
// (`cxxflag`) or cannot express the flag at all (`link-lib` emits `-l`,
// `link-search` emits `-L`) — so without it the one thing a user cannot write
// for themselves was the one thing they had to.

UTest link_script_becomes_dash_t_with_an_absolute_path{"BuildDirectives/LinkScriptBecomesDashTWithAnAbsolutePath", [] {
    auto d = parse("mcpp:link-script=board/link.ld\n");
    auto& ld = d.at(dirs::Slot::LdFlags);
    expect(fatal(eq(ld.size(), 1u)));
    // Absolute, because the link runs in the build directory: a relative path
    // resolves against the wrong root and lld answers "cannot find linker
    // script link.ld" (measured).
    expect(eq(ld[0], "-T " + under_root("board/link.ld")));
}};

UTest link_script_is_link_global_so_it_reaches_the_consumer{"BuildDirectives/LinkScriptIsLinkGlobalSoItReachesTheConsumer", [] {
    // The whole point: a dependency's build.mcpp must be able to widen the
    // LINK (as link-lib/link-search already do) without widening the public
    // COMPILE interface (which include-dir deliberately cannot).
    const dirs::Def* script = nullptr;
    const dirs::Def* incdir = nullptr;
    for (auto const& def : dirs::kTable) {
        if (def.wire == "link-script") script = &def;
        if (def.wire == "include-dir") incdir = &def;
    }
    expect(fatal(ne(script, nullptr)));
    expect(fatal(ne(incdir, nullptr)));
    expect(eq(script->scope, dirs::Scope::LinkGlobal));
    expect(eq(incdir->scope, dirs::Scope::PackagePrivate));
}};

UTest link_script_does_not_claim_a_declared_output{"BuildDirectives/LinkScriptDoesNotClaimADeclaredOutput", [] {
    // `mustExistAfterRun` assumes the value IS a path; this row's transformed
    // value is `-T <path>`, so the check would test the wrong string and
    // reject a script that is right there.
    for (auto const& def : dirs::kTable) {
        if (def.wire != "link-script") continue;
        expect(!def.mustExistAfterRun);
    }
}};

// ── runner ─────────────────────────────────────────────────────────────────
//
// How the artifact is EXECUTED, when the host cannot execute it. Produced by
// a board-support package's build.mcpp, because the value is machine-specific:
// the emulator lives in a package payload whose path carries a home and a
// version, and only a build program can compute that.

UTest runner_builds_an_argv_from_repeated_lines{"BuildDirectives/RunnerBuildsAnArgvFromRepeatedLines", [] {
    // argv is an ordered list and a directive is one line = one value, so the
    // list is built by repetition. Order is the emission order.
    auto d = parse("mcpp:runner=/payload/bin/qemu-system-riscv64\n"
                   "mcpp:runner=-machine\n"
                   "mcpp:runner=virt\n"
                   "mcpp:runner=-kernel\n");
    auto& r = d.at(dirs::Slot::Runner);
    expect(fatal(eq(r.size(), 4u)));
    expect(eq(r[0], "/payload/bin/qemu-system-riscv64"));
    expect(eq(r[1], "-machine"));
    expect(eq(r[3], "-kernel"));
}};

UTest runner_tokens_are_verbatim{"BuildDirectives/RunnerTokensAreVerbatim", [] {
    // Not a path to normalize: `-bios` is a token, and the producer already
    // resolved the executable absolutely (a bare name resolves through PATH to
    // a shim that dispatches against its OWNER home — measured in CI).
    auto d = parse("mcpp:runner=-bios\nmcpp:runner=none\n");
    auto& r = d.at(dirs::Slot::Runner);
    expect(fatal(eq(r.size(), 2u)));
    expect(eq(r[0], "-bios"));
    expect(eq(r[1], "none"));
}};

UTest runner_has_its_own_slot_and_scope{"BuildDirectives/RunnerHasItsOwnSlotAndScope", [] {
    const dirs::Def* runner = nullptr;
    const dirs::Def* script = nullptr;
    for (auto const& def : dirs::kTable) {
        if (def.wire == "runner")      runner = &def;
        if (def.wire == "link-script") script = &def;
    }
    expect(fatal(ne(runner, nullptr)));
    expect(fatal(ne(script, nullptr)));
    // Its own slot: putting it in LdFlags would put an emulator's argv on the
    // linker command line.
    expect(eq(runner->slot, dirs::Slot::Runner));
    expect(ne(runner->slot, script->slot));
    // Its own scope: RunGlobal propagates like LinkGlobal but carries an
    // exactly-one-provider rule that LinkGlobal must not inherit — two
    // dependencies' link flags concatenate correctly, two runners cannot.
    expect(eq(runner->scope, dirs::Scope::RunGlobal));
    expect(eq(script->scope, dirs::Scope::LinkGlobal));
}};

UTest runner_lands_in_build_config_not_in_ldflags{"BuildDirectives/RunnerLandsInBuildConfigNotInLdflags", [] {
    auto d = parse("mcpp:runner=/qemu\nmcpp:runner=-kernel\n"
                   "mcpp:link-lib=c\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.buildConfig.runner.size(), 2u)));
    expect(eq(m.buildConfig.runner[0], "/qemu"));
    // The link line must carry the library and nothing of the runner.
    for (auto const& f : m.buildConfig.ldflags)
        expect(eq(f.find("qemu"), std::string::npos)) << f;
}};

// ── Build-graph node decoding: `mcpp:action=` ───────────────────────────────
//
// `decode_action` is the other half of `mcpp::action::submit()` in
// src/build/hostprogram.cppm: the typed builder serialises one JSON object
// per action, and this is where it comes back. `depfile` is decoded the same
// way `blocking` is — a scalar read with `j.value(...)` and a default that
// matches "the field was never set" — so an action a build program submitted
// before this field existed decodes exactly as it always has.

UTest decode_action_round_trips_depfile{"BuildDirectives/DecodeActionRoundTripsDepfile", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"shader\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,\"depfile\":\"out/shader.spv.d\","
        "\"inputs\":[],\"outputs\":[\"out/shader.spv\"],"
        "\"command\":[\"glslc.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");

    expect(fatal(eq(d.at(dirs::Slot::Actions).size(), 1u)));
    auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(eq(a->depfile, "out/shader.spv.d"));
}};

// THE CONTROL — and the common case, since `depfile` is optional and
// `hostprogram.cppm`'s `submit()` omits the key entirely when it was never
// set (byte-identical payload to before the field existed). A decoder that
// required the key would reject every action submitted by that path.
UTest decode_action_defaults_depfile_to_empty_when_absent{"BuildDirectives/DecodeActionDefaultsDepfileToEmptyWhenAbsent", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"gen\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/gen.cpp\"],"
        "\"command\":[\"gen.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");

    expect(fatal(eq(d.at(dirs::Slot::Actions).size(), 1u)));
    auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(eq(a->depfile, ""));
}};

// The overflow marker. Until 2026.9.13.1 `mcpp::action` held its lists in
// fixed arrays and set the marker when a declaration did not fit; the lists
// now grow, and the marker means the build program could not allocate. The
// refusal stays -- an incomplete declaration must never be used -- and the
// message has to say what is now true: nothing about a buffer size, nothing
// recommending a response file for a list that no longer needs one.
UTest overflow_marker_is_refused_as_allocation_failure{"BuildDirectives/OverflowMarkerIsRefusedAsAllocationFailure", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"wide\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,\"overflow\":true,"
        "\"inputs\":[],\"outputs\":[\"out/a.txt\"],"
        "\"command\":[\"gen\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto err = dirs::action_error(d);
    expect(fatal(!err.empty()));
    expect(ne(err.find("could not be stored"), std::string::npos));
    expect(ne(err.find("out of memory"), std::string::npos));
    expect(eq(err.find("fixed buffer"), std::string::npos));
    expect(eq(err.find("response file"), std::string::npos));
    expect(eq(err.find("did not fit"), std::string::npos));
}};

// ── mcpp#702: the five action roles, and `output_dir` (`prepare` only) ─────
//
// `mcpp::roles::{source, check, object, artifact, prepare}` are the five
// spellings `decode_action` accepts. Before this, ANY unrecognised string
// decoded silently as Source; from here on `action_error` refuses one,
// naming the value and the five it knows, and `decode_action` refuses it too
// -- a cache-hit replay reaches the payload through `apply` alone, never
// through `action_error` (see the epoch-3 comment in program_protocol.cppm).

UTest decode_action_accepts_each_of_the_five_roles{"BuildDirectives/DecodeActionAcceptsEachOfTheFiveRoles", [] {
    struct Row {
        const char* role;
        mcpp::manifest::BuildAction::Role want;
        const char* extra;   // extra JSON fields `prepare` needs
    };
    const Row rows[] = {
        {"source",   mcpp::manifest::BuildAction::Role::Source,   ""},
        {"check",    mcpp::manifest::BuildAction::Role::Check,    ""},
        {"object",   mcpp::manifest::BuildAction::Role::Object,   ""},
        {"artifact", mcpp::manifest::BuildAction::Role::Artifact, ""},
        {"prepare",  mcpp::manifest::BuildAction::Role::Prepare,
         ",\"output_dir\":\"out/prep\""},
    };
    for (auto const& row : rows) {
        auto d = parse(std::format(
            "mcpp:action={{\"id\":\"a\",\"role\":\"{}\","
            "\"description\":\"\",\"blocking\":false{},"
            "\"inputs\":[],\"outputs\":[\"out/a\"],"
            "\"command\":[\"cmd\"],\"provides\":[],\"imports\":[],\"targets\":[]}}\n",
            row.role, row.extra));
        expect(fatal(eq(d.at(dirs::Slot::Actions).size(), 1u))) << row.role;
        auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
        expect(fatal(a.has_value())) << row.role;
        expect(eq(a->role, row.want)) << row.role;
    }
}};

// The default when `role` is absent is still `source`, exactly as it was
// before `prepare` and `output_dir` existed.
UTest decode_action_defaults_role_to_source_when_absent{"BuildDirectives/DecodeActionDefaultsRoleToSourceWhenAbsent", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"gen\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/gen.cpp\"],"
        "\"command\":[\"gen.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(eq(a->role, mcpp::manifest::BuildAction::Role::Source));
}};

UTest action_error_refuses_an_unknown_role_and_lists_the_five{"BuildDirectives/ActionErrorRefusesAnUnknownRoleAndListsTheFive", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"a\",\"role\":\"chek\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/a\"],"
        "\"command\":[\"cmd\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto err = dirs::action_error(d);
    expect(fatal(!err.empty()));
    expect(ne(err.find("\"chek\""), std::string::npos)) << err;
    for (auto const* role : {"source", "check", "object", "artifact", "prepare"})
        expect(ne(err.find(role), std::string::npos)) << err;
}};

UTest decode_action_refuses_an_unknown_role_even_outside_action_error{"BuildDirectives/DecodeActionRefusesAnUnknownRoleEvenOutsideActionError", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"a\",\"role\":\"chek\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/a\"],"
        "\"command\":[\"cmd\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    expect(!(dirs::decode_action(d.at(dirs::Slot::Actions).front()).has_value()));
}};

// `output_dir` round-trips like `depfile`: present when the typed builder
// set it, and read back verbatim.
UTest decode_action_round_trips_output_dir{"BuildDirectives/DecodeActionRoundTripsOutputDir", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"prep\",\"role\":\"prepare\","
        "\"description\":\"\",\"blocking\":false,\"output_dir\":\"out/prefix\","
        "\"inputs\":[],\"outputs\":[\"out/prep.stamp\"],"
        "\"command\":[\"install.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(eq(a->outputDir, "out/prefix"));
}};

// mcpp#708, protocol 13: `env` and `cwd` round-trip like `output_dir` --
// present when the typed builder set them, absent otherwise, and an action
// without them decodes exactly as it did under protocol 12.
UTest decode_action_round_trips_env_and_cwd{"BuildDirectives/DecodeActionRoundTripsEnvAndCwd", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"gen\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,"
        "\"env\":[\"GEN_MODE=fast\",\"EMPTY=\"],\"cwd\":\"third_party/gen\","
        "\"inputs\":[],\"outputs\":[\"out/gen.cpp\"],"
        "\"command\":[\"gen\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto a = dirs::decode_action(d.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(fatal(eq(a->env.size(), 2u)));
    expect(eq(a->env[0], "GEN_MODE=fast"));
    expect(eq(a->env[1], "EMPTY="));      // an empty value is a value
    expect(eq(a->cwd, "third_party/gen"));

    auto plain = parse(
        "mcpp:action={\"id\":\"gen\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/gen.cpp\"],"
        "\"command\":[\"gen\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    auto b = dirs::decode_action(plain.at(dirs::Slot::Actions).front());
    expect(fatal(b.has_value()));
    expect(b->env.empty());
    expect(b->cwd.empty());
}};

// An `env` entry with no name is refused, and the refusal names the entry.
UTest an_env_entry_without_a_name_is_refused{"BuildDirectives/AnEnvEntryWithoutANameIsRefused", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"gen\",\"role\":\"source\","
        "\"description\":\"\",\"blocking\":false,\"env\":[\"=oops\"],"
        "\"inputs\":[],\"outputs\":[\"out/gen.cpp\"],"
        "\"command\":[\"gen\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    expect(!(dirs::decode_action(d.at(dirs::Slot::Actions).front()).has_value()));
    const auto why = dirs::action_error(d);
    expect(ne(why.find("=oops"), std::string::npos)) << why;
    expect(ne(why.find("NAME=value"), std::string::npos)) << why;
}};

// A `prepare` action with no `output_dir` is refused: it would be a stamp
// and nothing else, indistinguishable from a `check` that forgot
// `blocking = true`.
UTest prepare_action_with_no_output_dir_is_refused{"BuildDirectives/PrepareActionWithNoOutputDirIsRefused", [] {
    auto d = parse(
        "mcpp:action={\"id\":\"prep\",\"role\":\"prepare\","
        "\"description\":\"\",\"blocking\":false,"
        "\"inputs\":[],\"outputs\":[\"out/prep.stamp\"],"
        "\"command\":[\"install.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}\n");
    expect(!(dirs::decode_action(d.at(dirs::Slot::Actions).front()).has_value()));
    expect(!(dirs::action_error(d).empty()));
}};

// Cache round trip: `mcpp:action=` is persisted (Scope::GraphNode), and a
// replayed `prepare` payload decodes to the same role and output_dir a fresh
// run would -- the cache hit must replay both exactly.
UTest prepare_action_survives_the_cache_round_trip{"BuildDirectives/PrepareActionSurvivesTheCacheRoundTrip", [] {
    // The paths are JSON strings: a Windows path's backslashes are escaped,
    // as the bundled module's serializer escapes them.
    auto json_string = [](std::string_view v) {
        std::string out;
        for (char c : v) {
            if (c == '\\' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    const std::string payload = std::format(
        "{{\"id\":\"prep\",\"role\":\"prepare\","
        "\"description\":\"\",\"blocking\":false,\"output_dir\":\"{}\","
        "\"inputs\":[],\"outputs\":[\"{}\"],"
        "\"command\":[\"install.sh\"],\"provides\":[],\"imports\":[],\"targets\":[]}}",
        json_string(under_root("prefix")), json_string(under_root("prep.stamp")));
    dirs::Directives replayed;
    expect(fatal(bool(dirs::accept_cache_record(replayed, "action", payload))));
    auto a = dirs::decode_action(replayed.at(dirs::Slot::Actions).front());
    expect(fatal(a.has_value()));
    expect(eq(a->role, mcpp::manifest::BuildAction::Role::Prepare));
    expect(eq(a->outputDir, under_root("prefix")));
}};

// ── #618: a named executable's subsystem and entry ──────────────────────────
//
// `windows-subsystem` and `windows-entry` name a target of the package being
// built. What is asserted: the value reaches that target's fields and nothing
// else, and every value `apply` could not honour is refused before it runs.

namespace {

mcpp::manifest::Manifest manifest_with_gui_and_core() {
    mcpp::manifest::Manifest m;
    m.package.name = "app";
    mcpp::manifest::Target gui;
    gui.name = "gui";
    gui.kind = mcpp::manifest::Target::Binary;
    mcpp::manifest::Target core;
    core.name = "core";
    core.kind = mcpp::manifest::Target::Library;
    m.targets = {gui, core};
    return m;
}

}  // namespace

UTest windows_subsystem_reaches_the_named_executable_only{"BuildDirectives/WindowsSubsystemReachesTheNamedExecutableOnly", [] {
    auto d = parse("mcpp:protocol=10\n"
                   "mcpp:windows-subsystem=gui:windows\n"
                   "mcpp:windows-entry=gui:wWinMain\n");
    expect(fatal(!(dirs::protocol_error(d).has_value())));
    auto m = manifest_with_gui_and_core();
    expect(fatal(eq(dirs::target_directive_error(m, d), "")));
    dirs::apply(m, d);
    expect(eq(m.targets[0].windowsSubsystem, "windows"));
    expect(eq(m.targets[0].windowsEntry, "wWinMain"));
    expect(m.targets[1].windowsSubsystem.empty());
    expect(m.targets[1].windowsEntry.empty());
    // Nothing reaches a flag channel another target or a consumer reads.
    expect(m.buildConfig.ldflags.empty());
    expect(m.buildConfig.cxxflags.empty());
}};

// #622 A3: `windows-subsystem`/`windows-entry` reach an `app` target exactly
// as they reach a `bin` one -- `directives.cppm`'s `apply` and
// `target_directive_error` ask `is_program()`, not `kind == Binary`.
UTest windows_subsystem_reaches_an_application_target_too{"BuildDirectives/WindowsSubsystemReachesAnApplicationTargetToo", [] {
    mcpp::manifest::Manifest m;
    m.package.name = "app";
    mcpp::manifest::Target myapp;
    myapp.name = "myapp";
    myapp.kind = mcpp::manifest::Target::Application;
    m.targets = {myapp};

    auto d = parse("mcpp:protocol=10\n"
                   "mcpp:windows-subsystem=myapp:windows\n"
                   "mcpp:windows-entry=myapp:wWinMain\n");
    expect(fatal(eq(dirs::target_directive_error(m, d), "")));
    dirs::apply(m, d);
    expect(eq(m.targets[0].windowsSubsystem, "windows"));
    expect(eq(m.targets[0].windowsEntry, "wWinMain"));
}};

UTest windows_subsystem_rows_have_the_target_link_scope{"BuildDirectives/WindowsSubsystemRowsHaveTheTargetLinkScope", [] {
    for (auto wire : {"windows-subsystem", "windows-entry"}) {
        auto def = dirs::find_by_wire(wire);
        expect(fatal(ne(def, nullptr))) << wire;
        expect(eq(def->scope, dirs::Scope::TargetLink)) << wire;
        expect(eq(def->sinceProtocol, 10)) << wire;
        expect(!(def->tag.empty())) << wire;
    }
}};

UTest windows_subsystem_refuses_what_apply_cannot_honour{"BuildDirectives/WindowsSubsystemRefusesWhatApplyCannotHonour", [] {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"mcpp:windows-subsystem=windows\n", "which is not `<target>:<value>`"},
        {"mcpp:windows-subsystem=gui:\n", "which is not `<target>:<value>`"},
        {"mcpp:windows-subsystem=gui:gui\n", "\"gui\" is not one of \"console\", \"windows\""},
        {"mcpp:windows-entry=gui:main2\n",
         "\"main2\" is not one of \"main\", \"wmain\", \"WinMain\", \"wWinMain\""},
        {"mcpp:windows-subsystem=nosuch:windows\n",
         "declares no target named `nosuch` (its targets: gui, core)"},
        {"mcpp:windows-entry=core:wmain\n", "target `core` is not one"},
        {"mcpp:windows-subsystem=gui:windows\nmcpp:windows-subsystem=gui:console\n",
         "twice for target `gui`, as \"windows\" and as \"console\""},
    };
    for (auto [text, expected] : cases) {
        auto d = parse(std::format("mcpp:protocol=10\n{}", text));
        auto err = dirs::target_directive_error(manifest_with_gui_and_core(), d);
        if (auto&& failed = expect(ne(err.find(expected), std::string::npos)); !failed) failed << text << " -> " << err;
    }
}};

UTest windows_subsystem_that_contradicts_the_manifest_is_refused{"BuildDirectives/WindowsSubsystemThatContradictsTheManifestIsRefused", [] {
    auto m = manifest_with_gui_and_core();
    m.targets[0].windowsSubsystem = "console";
    auto d = parse("mcpp:protocol=10\nmcpp:windows-subsystem=gui:windows\n");
    auto err = dirs::target_directive_error(m, d);
    expect(ne(err.find("[targets.gui] windows_subsystem = \"console\""), std::string::npos))
        << err;
    // Restating the manifest's own value is not a contradiction.
    m.targets[0].windowsSubsystem = "windows";
    expect(eq(dirs::target_directive_error(m, d), ""));
}};

// ── #622 A4: `mcpp::deploy(from, to)` ───────────────────────────────────────
//
// The build-program form of `[runtime] deploy` (#615): a file this program
// produced or selected, placed beside the artifact at `to`. What is asserted:
// `from` is resolved to an absolute path at PARSE time (so a package-relative
// `from` and an already-absolute one, an action's own declared output, land on
// the manifest the same way); `to` obeys the same rule the manifest key does,
// checked BEFORE `apply` so a bad value is refused on every replay, not only
// the run that first emitted it.

namespace {

mcpp::manifest::Manifest manifest_named(std::string name) {
    mcpp::manifest::Manifest m;
    m.package.name = std::move(name);
    return m;
}

}  // namespace

UTest deploy_row_is_protocol_eleven_with_link_global_scope_and_a_tag{"BuildDirectives/DeployRowIsProtocolElevenWithLinkGlobalScopeAndATag", [] {
    auto def = dirs::find_by_wire("deploy");
    expect(fatal(ne(def, nullptr)));
    expect(eq(def->scope, dirs::Scope::LinkGlobal));
    expect(eq(def->sinceProtocol, 11));
    expect(!(def->tag.empty()));
    expect(eq(dirs::kProtocolVersion, 18));
}};

UTest protocol_eighteen_is_accepted_and_nineteen_is_not{"BuildDirectives/ProtocolEighteenIsAcceptedAndNineteenIsNot", [] {
    auto ok = parse("mcpp:protocol=18\n");
    expect(!(dirs::protocol_error(ok).has_value()));
    auto no = parse("mcpp:protocol=19\n");
    expect(dirs::protocol_error(no).has_value());
}};

// SPEC-013 §4.1: the platform's fields a program states (protocol 17) become a
// platform statement at the build-program tier; malformed lines are refused.
UTest platform_fields_are_protocol_seventeen_and_reach_the_statement{"BuildDirectives/PlatformFieldsAreProtocolSeventeenAndReachTheStatement", [] {
    for (auto wire : {"link-driver", "link-input", "artifact-name", "seal", "run-host"}) {
        auto def = dirs::find_by_wire(wire);
        expect(fatal(ne(def, nullptr))) << wire;
        expect(eq(def->sinceProtocol, 17)) << wire;
        expect(!(def->tag.empty())) << wire;
    }
    mcpp::manifest::Manifest m;
    auto d = parse("mcpp:link-driver=lld\n"
                   "mcpp:link-input=from:notes.S:okld\n"
                   "mcpp:link-input=from:notes.S:note-source\n"
                   "mcpp:link-input=file:/opt/crt0.o\n"
                   "mcpp:artifact-name=exe:{name}.oke\n"
                   "mcpp:artifact-name=shared:lib{name}.okso\n"
                   "mcpp:seal=0:okld\nmcpp:seal=0:note\nmcpp:seal=0:{}\n"
                   "mcpp:seal=1:okld\nmcpp:seal=1:check\nmcpp:seal=1:{}\n"
                   "mcpp:run-host=same-arch\n");
    expect(eq(dirs::target_directive_error(m, d), ""));
    dirs::apply(m, d);
    auto const& s = m.buildConfig.programPlatform;
    expect(eq(s.link.driver.value, "lld"));
    expect(fatal(eq(s.link.inputs.size(), 2u)));
    expect(eq(s.link.inputs[0].as, "notes.S"));
    expect(eq(s.link.inputs[0].from, (std::vector<std::string>{"okld", "note-source"})));
    expect(eq(s.link.inputs[1].file, "/opt/crt0.o"));
    expect(eq(s.link.exe.value, "{name}.oke"));
    expect(eq(s.link.shared.value, "lib{name}.okso"));
    expect(fatal(eq(s.link.seal.size(), 2u)));
    expect(eq(s.link.seal[1], (std::vector<std::string>{"okld", "check", "{}"})));
    expect(eq(s.runner.host, "same-arch"));
    expect(eq(s.link.driver.origin.tier, mcpp::manifest::platform::Tier::Program));
    for (auto bad : {"mcpp:link-driver=gold\n", "mcpp:link-input=from:x\n",
                     "mcpp:artifact-name=exe:lib{name}\n", "mcpp:artifact-name=dll:{name}\n",
                     "mcpp:artifact-name=shared:lib/{name}.okso\n",
                     "mcpp:seal=x:okld\n", "mcpp:run-host=far\n"})
        expect(ne(dirs::target_directive_error(m, parse(bad)), "")) << bad;
}};

// Design 2026-10-26 §24.3: a board's image entry symbol reaches the manifest's
// link intent; a non-symbol and two different symbols are refused.
UTest link_start_is_protocol_sixteen_and_reaches_the_link_intent{"BuildDirectives/LinkStartIsProtocolSixteenAndReachesTheLinkIntent", [] {
    auto def = dirs::find_by_wire("link-start");
    expect(fatal(ne(def, nullptr)));
    expect(eq(def->scope, dirs::Scope::LinkGlobal));
    expect(eq(def->sinceProtocol, 16));
    mcpp::manifest::Manifest m;
    auto d = parse("mcpp:link-start=_reset\n");
    expect(eq(dirs::target_directive_error(m, d), ""));
    dirs::apply(m, d);
    expect(eq(m.runtimeConfig.linkIntent.start, "_reset"));
    expect(ne(dirs::target_directive_error(m, parse("mcpp:link-start=1bad\n")), ""));
    expect(ne(dirs::target_directive_error(
                  m, parse("mcpp:link-start=_reset\nmcpp:link-start=_start\n")), ""));
}};

UTest deploy_with_an_absolute_from_yields_one_entry_on_the_manifest{"BuildDirectives/DeployWithAnAbsoluteFromYieldsOneEntryOnTheManifest", [] {
    const std::string from = under_root("gen/x.bin");
    auto d = parse(std::format("mcpp:deploy={}\tres\n", from));
    auto m = manifest_named("app");
    expect(fatal(eq(dirs::deploy_directive_error(m, d), "")));
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.deploy.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.deploy[0].from.string(), from));
    expect(eq(m.runtimeConfig.linkIntent.deploy[0].to, "res"));
}};

// The manifest-sourced `from` rule (`deploy_path_problem`) refuses an absolute
// path. A directive's `from` is not run through that check at all: it is
// resolved to an absolute path by the table's own transform instead, because
// it may be an action's own declared output (#622 A4). This is the case that
// rule cannot see and must not reject.
UTest deploy_from_is_resolved_to_absolute_even_when_the_wire_value_already_was_one{"BuildDirectives/DeployFromIsResolvedToAbsoluteEvenWhenTheWireValueAlreadyWasOne", [] {
    const std::string absFrom = under_root("out/gen/res.bin");
    auto d = parse(std::format("mcpp:deploy={}\tapp.resources\n", absFrom));
    auto m = manifest_named("app");
    expect(eq(dirs::deploy_directive_error(m, d), ""));
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.deploy.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.deploy[0].from.string(), absFrom));
}};

// A relative `from` resolves against the package root, exactly as
// `include-dir`'s AbsPath case does — the directive never leaves a relative
// path for a later stage to guess the base of.
UTest deploy_from_relative_to_the_root_is_made_absolute{"BuildDirectives/DeployFromRelativeToTheRootIsMadeAbsolute", [] {
    auto d = parse("mcpp:deploy=gen/x.bin\tres\n");
    auto m = manifest_named("app");
    expect(fatal(eq(dirs::deploy_directive_error(m, d), "")));
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.deploy.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.deploy[0].from.string(), under_root("gen/x.bin")));
}};

// The negative direction (#622 A4, and rule 8 of the design record): a `to`
// `apply` cannot honour is refused BEFORE apply, naming the directive and the
// package -- not only the path problem, which `deploy_path_problem` already
// states on its own.
UTest deploy_to_dot_dot_is_refused_naming_the_directive_and_the_package{"BuildDirectives/DeployToDotDotIsRefusedNamingTheDirectiveAndThePackage", [] {
    auto d = parse(std::format("mcpp:deploy={}\t../x\n", under_root("gen/x.bin")));
    auto m = manifest_named("widget");
    auto err = dirs::deploy_directive_error(m, d);
    expect(ne(err.find("deploy"), std::string::npos)) << err;
    expect(ne(err.find("widget"), std::string::npos)) << err;
    expect(ne(err.find("`.` or `..` component"), std::string::npos)) << err;
}};

UTest deploy_with_a_malformed_wire_value_is_refused{"BuildDirectives/DeployWithAMalformedWireValueIsRefused", [] {
    auto d = parse("mcpp:deploy=onlyfromnotab\n");
    auto m = manifest_named("app");
    auto err = dirs::deploy_directive_error(m, d);
    expect(ne(err.find("deploy"), std::string::npos)) << err;
    expect(ne(err.find("not `<from>"), std::string::npos)) << err;
}};

// A `to` that IS honourable does not stop the build, and the collected entry
// is the only thing `apply` touches -- no flag channel gains anything, unlike
// nothing here (deploy is not a compile/link flag).
UTest deploy_reaches_only_the_runtime_deploy_list_not_any_flag_channel{"BuildDirectives/DeployReachesOnlyTheRuntimeDeployListNotAnyFlagChannel", [] {
    auto d = parse(std::format("mcpp:deploy={}\t.\n", under_root("gen/x.bin")));
    auto m = manifest_named("app");
    expect(fatal(eq(dirs::deploy_directive_error(m, d), "")));
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.deploy.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.deploy[0].to, "."));
    expect(m.buildConfig.ldflags.empty());
    expect(m.buildConfig.cxxflags.empty());
}};

// ── v12: `mcpp::runtime_search_dir(dir)` ────────────────────────────────────
//
// The build-program form of `runtime_search_dirs` (docs/04 §2.11): a
// launch-time search directory reached from a build.mcpp instead of TOML.
// What is asserted: it lands DIRECTLY on `LinkIntent::runtimeSearchDirs` --
// NOT the legacy `RuntimeConfig::libraryDirs`, which docs/04 §2.11 retires --
// which is what makes every existing consumer of that field (the plan merge,
// the RUNPATH rendering, `mcpp pack`'s closure search) see a
// directive-declared entry without any of them changing.

UTest runtime_search_dir_row_is_protocol_twelve_with_link_global_scope_and_a_tag{"BuildDirectives/RuntimeSearchDirRowIsProtocolTwelveWithLinkGlobalScopeAndATag", [] {
    auto def = dirs::find_by_wire("runtime-search-dir");
    expect(fatal(ne(def, nullptr)));
    expect(eq(def->scope, dirs::Scope::LinkGlobal));
    expect(eq(def->sinceProtocol, 12));
    expect(!(def->tag.empty()));
    expect(eq(def->transform, dirs::Transform::AbsPath));
}};

// The manifest key's own field, not a new one -- this is the whole point of
// the design: no downstream consumer has to learn a second field exists. NOT
// `RuntimeConfig::libraryDirs`, the retiring vector #702 as first written
// joined by mistake (design §5.3 point 1).
UTest runtime_search_dir_joins_link_intent_directly_not_the_legacy_vector{"BuildDirectives/RuntimeSearchDirJoinsLinkIntentDirectlyNotTheLegacyVector", [] {
    auto d = parse("mcpp:runtime-search-dir=rtlib\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[0].string(), under_root("rtlib")));
    expect(m.runtimeConfig.libraryDirs.empty());
}};

// Relative resolves against the package root, exactly as `include-dir`'s
// AbsPath case and `deploy`'s `from` do -- the directive never leaves a
// relative path for a later stage to guess the base of.
UTest runtime_search_dir_relative_to_the_root_is_made_absolute{"BuildDirectives/RuntimeSearchDirRelativeToTheRootIsMadeAbsolute", [] {
    auto d = parse("mcpp:runtime-search-dir=vendor/qt/bin\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[0].string(), under_root("vendor/qt/bin")));
}};

UTest runtime_search_dir_accepts_an_already_absolute_value{"BuildDirectives/RuntimeSearchDirAcceptsAnAlreadyAbsoluteValue", [] {
    const std::string abs = under_root("prefix/lib");
    auto d = parse(std::format("mcpp:runtime-search-dir={}\n", abs));
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[0].string(), abs));
}};

// Reaches only the run-time search field -- no compile or link flag channel
// gains anything, unlike `link-search`, which shares the manifest's -L/-L
// distinction.
UTest runtime_search_dir_reaches_no_flag_channel{"BuildDirectives/RuntimeSearchDirReachesNoFlagChannel", [] {
    auto d = parse("mcpp:runtime-search-dir=rtlib\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(m.buildConfig.ldflags.empty());
    expect(m.buildConfig.cxxflags.empty());
    expect(m.runtimeConfig.linkIntent.linkLibraryDirs.empty());
}};

// Multiple directives accumulate, in emission order, the same as every other
// repeated directive in this table.
UTest runtime_search_dir_directives_accumulate{"BuildDirectives/RuntimeSearchDirDirectivesAccumulate", [] {
    auto d = parse("mcpp:runtime-search-dir=a\n"
                   "mcpp:runtime-search-dir=b\n");
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs.size(), 2u)));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[0].string(), under_root("a")));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[1].string(), under_root("b")));
}};

// Persisted like `deploy`, `warning` and `pack-format`: the cache tag is
// non-empty and round-trips through the same table-driven serialize/
// accept_cache_record pair, so a build.mcpp CACHE HIT replays the directive
// rather than losing it on every build after the first.
UTest runtime_search_dir_is_persisted_and_round_trips{"BuildDirectives/RuntimeSearchDirIsPersistedAndRoundTrips", [] {
    auto d = parse("mcpp:runtime-search-dir=rtlib\n");
    std::ostringstream os;
    dirs::serialize(os, d);
    expect(ne(os.str().find("d runtime-search-dir "), std::string::npos)) << os.str();

    dirs::Directives replayed;
    expect(fatal(bool(dirs::accept_cache_record(replayed, "runtime-search-dir",
                                          under_root("rtlib")))));
    mcpp::manifest::Manifest m;
    dirs::apply(m, replayed);
    expect(fatal(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs.size(), 1u)));
    expect(eq(m.runtimeConfig.linkIntent.runtimeSearchDirs[0].string(), under_root("rtlib")));
}};

UTest optional_action_progress_defaults_off_and_rejects_wrong_type{"BuildDirectives/OptionalActionProgressDefaultsOffAndRejectsWrongType", [] {
    const std::string prefix=R"({"id":"work","role":"object","command":["worker"],"outputs":["result.o"])";
    const auto old=dirs::decode_action(prefix+"}");
    expect(fatal(bool(old))); expect(!old->progress);
    const auto opted=dirs::decode_action(prefix+R"(,"progress":true})");
    expect(fatal(bool(opted))); expect(bool(opted->progress));
    expect(!(dirs::decode_action(prefix+R"(,"progress":"true"})")));
}};
