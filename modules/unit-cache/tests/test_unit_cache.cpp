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
    expect(!uc::shape_of({"/store/g++", "-c", "a.cpp", "-o", "a.o"})) << "no depfile";
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
