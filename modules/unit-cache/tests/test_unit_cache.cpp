import std;
import mcpplibs.mtest;
import mcpp.build.unit_cache;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: a command's shape, a depfile's prerequisites, and a compile kept and served
// again by the contents of what it read (design 2026-11-20 §5.3). That the executor serves a
// fresh checkout's units from it is stated by the e2e suite (tests/e2e/cache/).

namespace uc = mcpp::build::unit_cache;
namespace fs = std::filesystem;

namespace {
struct Tmp {
    fs::path dir = fs::temp_directory_path() / std::format("mcpp-unit-cache-{:08x}", std::random_device{}());
    Tmp() { fs::create_directories(dir); }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

UTest a_command_states_its_source_and_depfile{"UnitCache/ACommandStatesItsSourceAndDepfile", [] {
    auto s = uc::shape_of({"/store/g++", "-isystem", "/store/inc", "-fmodule-mapper=modmap/p.map",
                           "@obj/a.rsp", "-MMD", "-MF", "obj/a.o.d.raw", "-gsplit-dwarf", "-c", "/w/a.cpp", "-o", "obj/a.o"});
    expect(fatal(s.has_value()));
    expect(eq(s->source, fs::path("/w/a.cpp")));
    expect(eq(s->depfile, fs::path("obj/a.o.d.raw")));
    expect(eq(s->mapper, fs::path("modmap/p.map")));
    expect(eq(s->responses.size(), std::size_t{1}));
    expect(eq(s->systemDirs.front(), fs::path("/store/inc")));
    expect(s->splitDebug);
    expect(uc::shape_of({"/store/g++", "-c", "a.cpp", "-o", "a.o"})->depfile.empty()) << "no depfile";
    expect(!uc::shape_of({"/store/g++", "-o", "a.o"})) << "no source";
    expect(!uc::shape_of({"/store/g++", "-fmodule-mapper=|mapper", "-MF", "d", "-c", "a.cpp"})) << "a mapper program";
}};

UTest a_depfile_lists_every_rules_prerequisites{"UnitCache/ADepfileListsEveryRulesPrerequisites", [] {
    const auto p = uc::depfile_prerequisites(
        "obj/a.o: /w/src/a.cpp /w/inc/a\\ b.h \\\n  gcm.cache/x.gcm\nx.c++m: obj/a.o\nobj/a.o: /w/src/a.cpp C:/w/c.h\n");
    expect(eq(p, std::vector<std::string>{"/w/src/a.cpp", "/w/inc/a b.h", "gcm.cache/x.gcm", "obj/a.o", "C:/w/c.h"}));
}};

UTest a_compile_is_served_while_what_it_read_holds{"UnitCache/ACompileIsServedWhileWhatItReadHolds", [] {
    Tmp t;
    const auto store = t.dir / "store", work = t.dir / "work", build = work / "target";
    write(store / "bin/g++", "compiler");
    write(work / "src/a.cpp", "int a();");
    write(work / "inc/a.h", "#define A 1");
    const std::vector<std::string> argv = {(store / "bin/g++").string(), "-MMD", "-MF", "obj/a.o.d.raw", "-c",
                                           (work / "src/a.cpp").string(), "-o", "obj/a.o"};
    const uc::Unit unit{argv, build, "obj/a.o", {}};
    uc::Cache cache(t.dir / "cache", {store}, build);
    expect(eq(cache.refusal(unit), std::string{}));
    expect(!cache.serve(unit)) << "an empty cache served";
    write(build / "obj/a.o", "OBJECT-1");
    write(build / "obj/a.o.d.raw", std::format("obj/a.o: {} {} {}\nobj/a.o: x.c++-module\nCXX_IMPORTS += x.c++-module\n",
                                               (work / "src/a.cpp").generic_string(), (work / "inc/a.h").generic_string(),
                                               (store / "inc/std.h").generic_string()));
    cache.keep(unit);
    fs::remove(build / "obj/a.o");
    fs::remove(build / "obj/a.o.d.raw");
    expect(fatal(cache.serve(unit))) << "the kept compile was not served";
    expect(eq(read(build / "obj/a.o"), std::string("OBJECT-1")));
    expect(read(build / "obj/a.o.d.raw").contains("inc/a.h"));
    // A header it read changed: another compile.
    write(work / "inc/a.h", "#define A 2");
    uc::Cache again(t.dir / "cache", {store}, build);
    expect(!again.serve(unit)) << "a changed header was served";
    // A compiler outside the stores is refused.
    const uc::Unit host{{"/usr/bin/g++", "-MF", "d", "-c", "a.cpp", "-o", "a.o"}, build, "a.o", {}};
    expect(ne(cache.refusal(host), std::string{}));
}};

UTest an_importer_names_a_bmi_by_its_producer{"UnitCache/AnImporterNamesABmiByItsProducer", [] {
    // A GCC BMI is not the same bytes from one compile to the next: the importer's manifest names
    // it by the key of the compile that wrote it, so a BMI written again from the same inputs
    // serves the importer.
    Tmp t;
    const auto store = t.dir / "store", build = t.dir / "build";
    write(store / "bin/g++", "compiler");
    write(t.dir / "src/m.cppm", "export module m;");
    write(t.dir / "src/user.cpp", "import m;");
    uc::Cache cache(t.dir / "cache", {store}, build);
    const uc::Unit module{{(store / "bin/g++").string(), "-MF", "obj/m.o.d", "-c", (t.dir / "src/m.cppm").string(), "-o", "obj/m.o"},
                          build, "obj/m.o", "gcm.cache/m.gcm"};
    write(build / "obj/m.o", "M");
    write(build / "gcm.cache/m.gcm", "BMI-1");
    write(build / "obj/m.o.d", std::format("obj/m.o gcm.cache/m.gcm: {}\n", (t.dir / "src/m.cppm").generic_string()));
    cache.keep(module);
    expect(fatal(fs::exists(build / "gcm.cache/m.gcm.mcpp-unit"))) << "the BMI has no producer key";
    const uc::Unit user{{(store / "bin/g++").string(), "-MF", "obj/user.o.d", "-c", (t.dir / "src/user.cpp").string(), "-o", "obj/user.o"},
                        build, "obj/user.o", {}};
    write(build / "obj/user.o", "U");
    write(build / "obj/user.o.d", std::format("obj/user.o: {} gcm.cache/m.gcm\n", (t.dir / "src/user.cpp").generic_string()));
    cache.keep(user);
    // The module compiled again from the same inputs: other bytes, the same producer.
    write(build / "gcm.cache/m.gcm", "BMI-2");
    fs::remove(build / "obj/user.o");
    uc::Cache again(t.dir / "cache", {store}, build);
    expect(again.serve(user)) << "a BMI written again from the same inputs did not serve its importer";
    // A BMI whose producer is not the cache's is named by its bytes.
    uc::Cache::forget(module);
    uc::Cache third(t.dir / "cache", {store}, build);
    expect(!third.serve(user)) << "a BMI without its producer's key served by name";
}};

UTest a_unit_travels_as_a_record_and_its_imports_are_read_from_the_scan{"UnitCache/AUnitTravelsAsARecord", [] {
    uc::Unit u{uc::split_command(R"(/store/g++ -x c++ "-DNAME=a b" -c /w/a.cppm -o obj/a.o)"), "/build", "obj/a.o", "gcm.cache/a.gcm"};
    expect(eq(u.argv[3], std::string("-DNAME=a b")));
    u.reads = std::vector<fs::path>{"/w/a.cppm", "gcm.cache/b.gcm"};
    u.placeBmi = ".mcpp-attempts/1/out/gcm.cache/a.gcm";
    const auto back = uc::from_record(uc::to_record(u));
    expect(fatal(back.has_value()));
    expect(eq(back->argv, u.argv));
    expect(eq(back->placeBmi, u.placeBmi));
    expect(fatal(back->reads.has_value()));
    expect(eq(back->reads->size(), std::size_t{2}));
    const auto bmis = uc::imported_bmis(R"({"rules":[{"provides":[{"logical-name":"a"}],"requires":[{"logical-name":"b"},{"logical-name":"std"}]}]})",
                                        "$root gcm.cache\nb b.gcm\nstd std.gcm\na a.gcm\n", "/build");
    expect(eq(bmis, std::vector<fs::path>{"/build/gcm.cache/b.gcm", "/build/gcm.cache/std.gcm"}));
}};

UTest a_stamp_is_compared_by_time_then_content{"UnitCache/AStampIsComparedByTimeThenContent", [] {
    Tmp t;
    const auto f = t.dir / "emit.hpp";
    write(f, "one");
    // A file older than the racy window: its time identifies its content.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours{1};
    fs::last_write_time(f, old);
    auto s = uc::stamp_of(f);
    expect(fatal(s.has_value()));
    expect(s->mtime != 0);
    expect(uc::same_by_stat(f, *s));
    expect(uc::unchanged(f, *s));

    // Touched, same content: unchanged, and the stamp to keep is handed back.
    fs::last_write_time(f, old + std::chrono::minutes{1});
    std::optional<uc::Stamp> refreshed;
    expect(!uc::same_by_stat(f, *s));
    expect(uc::unchanged(f, *s, &refreshed));
    expect(fatal(refreshed.has_value()));
    expect(uc::same_by_stat(f, *refreshed));

    // Same size and the same time, other content: a stamp that keeps a time trusts it, which is
    // why a racily new file keeps none.
    write(f, "two");
    expect(eq(uc::stamp_of(f)->mtime, std::int64_t{0})) << "a file written now keeps no time";
    expect(!uc::unchanged(f, *refreshed));
    fs::remove(f);
    expect(!uc::unchanged(f, *refreshed)) << "a file that is gone is changed";
}};

UTest a_racily_new_stamp_is_compared_by_content{"UnitCache/ARacilyNewStampIsComparedByContent", [] {
    Tmp t;
    const auto f = t.dir / "a.txt";
    write(f, "abc");
    const auto s = uc::stamp_of(f);
    expect(fatal(s.has_value()));
    expect(eq(s->mtime, std::int64_t{0}));
    expect(!uc::same_by_stat(f, *s)) << "no time, so the fast reader cannot tell";
    expect(uc::unchanged(f, *s));
    write(f, "abd");
    expect(!uc::unchanged(f, *s));
}};
