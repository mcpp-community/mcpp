import std;
import mcpplibs.mtest;
import mcpp.build.schedule.relocation;

using namespace mcpplibs::mtest;

namespace rl = mcpp::build::schedule::relocation;
namespace fs = std::filesystem;

namespace {
// A build directory of its own, entered for the case: the executor runs where
// ninja does, and spells the attempt's paths relative to it.
struct BuildDir {
    fs::path root, before;
    BuildDir() {
        root = fs::temp_directory_path() / std::format("mcpp-relocation-{:08x}", std::random_device{}());
        fs::create_directories(root);
        before = fs::current_path();
        fs::current_path(root);
    }
    ~BuildDir() {
        std::error_code ec;
        fs::current_path(before, ec);
        fs::remove_all(root, ec);
    }
    static void write(const fs::path& p, std::string_view text) {
        fs::create_directories(p.parent_path().empty() ? fs::path(".") : p.parent_path());
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    }
    static std::string read(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};
}  // namespace

UTest words_naming_an_output_are_written_below_the_attempt{"Relocation/WordsNamingAnOutputAreWrittenBelowTheAttempt", [] {
    BuildDir d;
    std::vector<std::string> argv{"clang++", "-c", "src/x.cppm", "-o", "obj/p/x.m.o", "-MMD", "-MF", "obj/p/x.m.o.d.raw",
                                  "@obj/p/x.m.o.rsp", "-fmodule-output=pcm.cache/p/x.pcm", "-Iobj/p/x.m.o.inc"};
    auto plan = rl::relocate(argv, {"obj/p/x.m.o", "obj/p/x.m.o.d", "pcm.cache/p/x.pcm", true}, "a1", "dumpdir");
    expect(fatal(bool(plan) && plan->relocated()));
    const std::vector<std::string> want{"clang++", "-c", "src/x.cppm", "-o", ".mcpp-attempts/a1/out/obj/p/x.m.o", "-MMD", "-MF",
                                        ".mcpp-attempts/a1/out/obj/p/x.m.o.d.raw", "@obj/p/x.m.o.rsp",
                                        "-fmodule-output=.mcpp-attempts/a1/out/pcm.cache/p/x.pcm", "-Iobj/p/x.m.o.inc",
                                        "-dumpdir", "obj/p/x.m."};
    expect(eq(argv, want));
    expect(eq(plan->outputs.size(), 3u));
    // Kinds decide publication: an object and a BMI equal to the published one are kept.
    expect(plan->outputs[0].kind == rl::Output::Kind::object);
    expect(plan->outputs[1].kind == rl::Output::Kind::depfile);
    expect(plan->outputs[2].kind == rl::Output::Kind::bmi);
    expect(fs::is_directory(".mcpp-attempts/a1/out/obj/p")) << "the directory of the attempt's -o was not made";
}};

UTest a_gcc_module_map_names_the_attempts_bmi{"Relocation/AGccModuleMapNamesTheAttemptsBmi", [] {
    BuildDir d;
    BuildDir::write("modmap/p-0011aabb.map", "$root gcm.cache\nx p/d1/x.gcm\ny y.gcm\n");
    std::vector<std::string> argv{"g++", "-fmodule-mapper=modmap/p-0011aabb.map", "-c", "x.cppm", "-o", "obj/p/d1/x.m.o"};
    auto plan = rl::relocate(argv, {"obj/p/d1/x.m.o", "", "gcm.cache/p/d1/x.gcm", true}, "a2", "dumpbase");
    expect(fatal(bool(plan) && plan->relocated()));
    expect(eq(argv[1], std::string("-fmodule-mapper=.mcpp-attempts/a2/module.map")));
    // Its own module below the attempt, relative to the repository; its imports where they are.
    expect(eq(BuildDir::read(".mcpp-attempts/a2/module.map"),
              std::string("$root gcm.cache\nx ../.mcpp-attempts/a2/out/gcm.cache/p/d1/x.gcm\ny y.gcm\n")));
    expect(eq(BuildDir::read("modmap/p-0011aabb.map"), std::string("$root gcm.cache\nx p/d1/x.gcm\ny y.gcm\n")));
    // The auxiliary outputs keep the names GCC derives from the final object.
    const std::vector<std::string> tail(argv.end() - 4, argv.end());
    expect(eq(tail, std::vector<std::string>{"-dumpdir", "obj/p/d1/", "-dumpbase", "x.m"}));
    expect(eq(rl::written_bmi(*plan).generic_string(), std::string(".mcpp-attempts/a2/out/gcm.cache/p/d1/x.gcm")));
}};

UTest a_compile_that_cannot_be_relocated_stays_in_place{"Relocation/ACompileThatCannotBeRelocatedStaysInPlace", [] {
    BuildDir d;
    // An output outside the build directory, and a map that does not name the BMI: every
    // output below the attempt or none, so the command is as it was.
    const auto outside = (fs::temp_directory_path() / "elsewhere/x.o").string();
    std::vector<std::string> argv{"g++", "-c", "x.cpp", "-o", outside};
    const auto before = argv;
    auto plan = rl::relocate(argv, {outside, "", "", true}, "a3", "dumpbase");
    expect(fatal(bool(plan)));
    expect(!plan->relocated());
    expect(eq(argv, before));
    BuildDir::write("modmap/q.map", "$root gcm.cache\nother other.gcm\n");
    std::vector<std::string> mapped{"g++", "-fmodule-mapper=modmap/q.map", "-c", "x.cppm", "-o", "obj/x.o"};
    const auto beforeMapped = mapped;
    plan = rl::relocate(mapped, {"obj/x.o", "", "gcm.cache/x.gcm", true}, "a4", "dumpbase");
    expect(fatal(bool(plan)));
    expect(!plan->relocated());
    expect(eq(mapped, beforeMapped));
    expect(!fs::exists(".mcpp-attempts/a4")) << "a compile in place left an attempt directory";
}};

UTest publication_keeps_an_equal_output_and_replaces_another{"Relocation/PublicationKeepsAnEqualOutputAndReplacesAnother", [] {
    BuildDir d;
    std::vector<std::string> argv{"g++", "-c", "x.cpp", "-o", "obj/x.o", "-MMD", "-MF", "obj/x.o.d"};
    auto plan = rl::relocate(argv, {"obj/x.o", "obj/x.o.d", "", true}, "a5", "");
    expect(fatal(bool(plan) && plan->relocated()));
    BuildDir::write("obj/x.o", "object bytes");
    const auto old = fs::last_write_time("obj/x.o") - std::chrono::hours(1);
    fs::last_write_time("obj/x.o", old);
    BuildDir::write(".mcpp-attempts/a5/out/obj/x.o", "object bytes");
    BuildDir::write(".mcpp-attempts/a5/out/obj/x.o.d", "obj/x.o: x.cpp\n");
    BuildDir::write(".mcpp-attempts/a5/out/obj/x.json", "{}");   // named after the object by the compiler
    expect(fatal(bool(rl::publish(*plan))));
    // The equal object stays as it was, its time unchanged: `restat` ends the rebuild there.
    expect(eq(BuildDir::read("obj/x.o"), std::string("object bytes")));
    expect(fs::last_write_time("obj/x.o") == old);
    expect(eq(BuildDir::read("obj/x.o.d"), std::string("obj/x.o: x.cpp\n")));
    expect(eq(BuildDir::read("obj/x.json"), std::string("{}")));
    BuildDir::write(".mcpp-attempts/a5/out/obj/x.o", "other bytes");
    expect(fatal(bool(rl::publish(*plan))));
    expect(eq(BuildDir::read("obj/x.o"), std::string("other bytes")));
    rl::discard(plan->directory);
    expect(!fs::exists(".mcpp-attempts/a5"));
}};

UTest a_shell_command_is_rewritten_word_by_word{"Relocation/AShellCommandIsRewrittenWordByWord", [] {
    BuildDir d;
    std::string command = "g++ -I\"/a b/obj/x.o\"  -c x.cpp -o obj/x.o";
    auto plan = rl::relocate(command, {"obj/x.o", "", "", true}, "a6", "dumpbase");
    expect(fatal(bool(plan) && plan->relocated()));
    expect(eq(command, std::string("g++ -I\"/a b/obj/x.o\"  -c x.cpp -o .mcpp-attempts/a6/out/obj/x.o -dumpdir obj/ -dumpbase x")));
}};

UTest a_saved_plan_reads_back{"Relocation/ASavedPlanReadsBack", [] {
    BuildDir d;
    std::vector<std::string> argv{"g++", "-c", "x.cpp", "-o", "obj/x.o", "-MF", "obj/x.o.d"};
    auto plan = rl::relocate(argv, {"obj/x.o", "obj/x.o.d", "", true}, "a7", "");
    expect(fatal(bool(plan) && bool(rl::save(*plan))));
    auto loaded = rl::load(plan->directory);
    expect(fatal(bool(loaded)));
    expect(eq(loaded->outputs.size(), plan->outputs.size()));
    for (std::size_t i = 0; i < plan->outputs.size() && i < loaded->outputs.size(); ++i) {
        expect(loaded->outputs[i].kind == plan->outputs[i].kind);
        expect(eq(loaded->outputs[i].published, plan->outputs[i].published));
        expect(eq(loaded->outputs[i].written, plan->outputs[i].written));
    }
}};

UTest a_published_dependency_file_names_what_is_published{"Relocation/APublishedDependencyFileNamesWhatIsPublished", [] {
    BuildDir d;
    // GCC lists its module mapper among a unit's prerequisites, and names the attempt's object as
    // the target: published as they were, every later build found the attempt's map missing and
    // compiled the unit again.
    BuildDir::write("modmap/p.map", "$root gcm.cache\nx x.gcm\n");
    std::vector<std::string> argv{"g++", "-fmodule-mapper=modmap/p.map", "-c", "x.cppm", "-o", "obj/x.m.o",
                                  "-MMD", "-MF", "obj/x.m.o.d.raw"};
    auto plan = rl::relocate(argv, {"obj/x.m.o", "obj/x.m.o.d", "gcm.cache/x.gcm", true}, "a8", "dumpbase");
    expect(fatal(bool(plan) && plan->relocated()));
    BuildDir::write(".mcpp-attempts/a8/out/obj/x.m.o.d.raw",
                    ".mcpp-attempts/a8/out/obj/x.m.o .mcpp-attempts/a8/out/gcm.cache/x.gcm: x.cppm \\\n"
                    " .mcpp-attempts/a8/module.map\n");
    BuildDir::write(".mcpp-attempts/a8/out/obj/x.m.o", "o");
    BuildDir::write(".mcpp-attempts/a8/out/gcm.cache/x.gcm", "b");
    expect(fatal(bool(rl::save(*plan))));
    auto loaded = rl::load(plan->directory);
    expect(fatal(bool(loaded) && bool(rl::publish(*loaded))));
    expect(eq(BuildDir::read("obj/x.m.o.d.raw"), std::string("obj/x.m.o gcm.cache/x.gcm: x.cppm \\\n modmap/p.map\n")));
    expect(eq(BuildDir::read("gcm.cache/x.gcm"), std::string("b")));
}};
