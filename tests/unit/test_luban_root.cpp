// A Luban system says what it is (xlings design 2026-10-21 §3.7, J5): its
// root is declared (os-release ID=luban and /usr/lib/xlings-abi), so its
// compiler's sysroot `/` is not foreign there; and its store is the home its
// root record names, which `[xlings] upstream = ["/xlings"]` means for a
// system made on a host (whose home is not at /xlings).


import std;
import mcpplibs.mtest;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.fallback.probe_sysroot;

using namespace mcpplibs::mtest;

namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path dir;
    Tree() {
        dir = fs::temp_directory_path() / std::format("mcpp-luban-root-{}-{}", mcpplibs::mtest::seed(),
                                                      std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(dir);
    }
    ~Tree() { std::error_code ec; fs::remove_all(dir, ec); }
    void write(const fs::path& rel, std::string_view text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel) << text;
    }
};

}  // namespace

UTest a_luban_systems_root_is_declared{"LubanRoot/ALubanSystemsRootIsDeclared", [] {
    Tree t;
    expect(!(mcpp::provider::layout().declared_system_root(t.dir)));
    t.write("etc/os-release", "NAME=\"Luban\"\nID=luban\nVERSION_ID=2026.10.22.1\n");
    expect(!(mcpp::provider::layout().declared_system_root(t.dir))) << "its os-release alone is not enough";
    t.write("usr/lib/xlings-abi/README", "");
    expect(bool(mcpp::provider::layout().declared_system_root(t.dir)));
    t.write("etc/os-release", "ID=\"debian\"\n");
    expect(!(mcpp::provider::layout().declared_system_root(t.dir)));
}};

UTest its_sysroot_slash_is_not_foreign{"LubanRoot/ItsSysrootSlashIsNotForeign", [] {
    Tree t;
    const fs::path registry = "/home/u/.mcpp/registry", project = "/home/u/work/demo";
    expect(mcpp::fallback::sysroot_is_foreign("/", registry, project, t.dir)) << "anywhere else, / is nothing declared";
    t.write("etc/os-release", "ID=luban\n");
    t.write("usr/lib/xlings-abi/README", "");
    expect(!mcpp::fallback::sysroot_is_foreign("/", registry, project, t.dir));
    expect(mcpp::fallback::sysroot_is_foreign("/opt/other/sysroot", registry, project, t.dir)) << "only its own root";
}};

UTest its_store_is_the_home_its_root_record_names{"LubanRoot/ItsStoreIsTheHomeItsRootRecordNames", [] {
    Tree t;
    t.write("etc/os-release", "ID=luban\n");
    t.write("usr/lib/xlings-abi/README", "");
    expect(mcpp::provider::layout().system_home(t.dir).empty()) << "no record";
    const auto home = t.dir / "host-home";
    fs::create_directories(home / "subos" / "box" / "root" / "usr");
    t.write("etc/xlings/usr", (home / "subos" / "box" / "root" / "usr").string() + "\n");
    expect(eq(mcpp::provider::layout().system_home(t.dir), home.lexically_normal()));
    t.write("etc/xlings/usr", "/somewhere/else/usr\n");
    expect(mcpp::provider::layout().system_home(t.dir).empty()) << "a record of another shape";
}};
