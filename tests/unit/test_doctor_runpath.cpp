
import std;
import mcpplibs.mtest;
import mcpp.doctor;

using namespace mcpplibs::mtest;

using mcpp::doctor::parse_readelf_runpath;

// `readelf -d` line shape (the case that motivated the check): clang++ with a
// RUNPATH that includes a now-removed xim-x-zlib lib dir.
UTest parses_runpath_colon_separated_dirs{"DoctorRunpath/ParsesRunpathColonSeparatedDirs", [] {
    std::string dump =
        " 0x0000000000000001 (NEEDED)             Shared library: [libz.so.1]\n"
        " 0x000000000000001d (RUNPATH)            Library runpath: "
        "[/home/u/.mcpp/data/xpkgs/xim-x-llvm/20.1.7/lib:"
        "/home/u/.mcpp/data/xpkgs/xim-x-zlib/1.3.1/lib:"
        "/home/u/.mcpp/registry/subos/default/lib]\n"
        " 0x000000000000000c (INIT)               0x1000\n";

    auto dirs = parse_readelf_runpath(dump);
    expect(fatal(eq(dirs.size(), 3u)));
    expect(eq(dirs[0], "/home/u/.mcpp/data/xpkgs/xim-x-llvm/20.1.7/lib"));
    expect(eq(dirs[1], "/home/u/.mcpp/data/xpkgs/xim-x-zlib/1.3.1/lib"));
    expect(eq(dirs[2], "/home/u/.mcpp/registry/subos/default/lib"));
}};

// DT_RPATH (legacy) is parsed the same way as DT_RUNPATH.
UTest parses_legacy_rpath{"DoctorRunpath/ParsesLegacyRpath", [] {
    std::string dump =
        " 0x000000000000000f (RPATH)              Library rpath: [/opt/a/lib:/opt/b/lib]\n";
    auto dirs = parse_readelf_runpath(dump);
    expect(fatal(eq(dirs.size(), 2u)));
    expect(eq(dirs[0], "/opt/a/lib"));
    expect(eq(dirs[1], "/opt/b/lib"));
}};

// A binary with no RUNPATH/RPATH entry yields no dirs.
UTest no_runpath_yields_empty{"DoctorRunpath/NoRunpathYieldsEmpty", [] {
    std::string dump =
        " 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]\n"
        " 0x000000000000000c (INIT)               0x1000\n";
    expect(parse_readelf_runpath(dump).empty());
}};

// Empty path tokens (e.g. a trailing ':') are dropped, not reported as a
// missing dir.
UTest drops_empty_tokens{"DoctorRunpath/DropsEmptyTokens", [] {
    std::string dump =
        " 0x000000000000001d (RUNPATH)            Library runpath: [/a/lib::/b/lib:]\n";
    auto dirs = parse_readelf_runpath(dump);
    expect(fatal(eq(dirs.size(), 2u)));
    expect(eq(dirs[0], "/a/lib"));
    expect(eq(dirs[1], "/b/lib"));
}};
