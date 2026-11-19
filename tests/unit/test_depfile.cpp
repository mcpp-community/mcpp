// mcpp.build.depfile: the first record of a GNU depfile, which
// `mcpp depfile-filter` keeps on Windows where POSIX runs the awk program
// `NR==1{print;next} /^[^ ]/{exit} {print}` (ninja_backend.cppm, WS2).

import std;
import mcpplibs.mtest;
import mcpp.build.depfile;

using namespace mcpplibs::mtest;

using mcpp::build::depfile::first_record;

// GCC 16 with -fmodules, for a module interface with a purview #include: the
// first record is the textual include graph, and the records after it (the
// BMI having inputs, the phony module target) are what ninja rejects.
UTest keeps_the_include_graph_and_drops_gccs_module_records{"Depfile/KeepsTheIncludeGraphAndDropsGccsModuleRecords", [] {
    const std::string raw =
        "obj/m.m.o gcm.cache/m.gcm: src/m.cppm \\\n"
        " src/vals.inc\n"
        "m.c++-module: gcm.cache/m.gcm\n"
        ".PHONY: m.c++-module\n"
        "gcm.cache/m.gcm:| obj/m.m.o\n";
    expect(eq(first_record(raw), "obj/m.m.o gcm.cache/m.gcm: src/m.cppm \\\n"
              " src/vals.inc\n"));
}};

UTest an_importing_unit_keeps_its_first_record_only{"Depfile/AnImportingUnitKeepsItsFirstRecordOnly", [] {
    const std::string raw =
        "obj/main.o: src/main.cpp gcm.cache/std.gcm gcm.cache/m.gcm\n"
        "obj/main.o: m.c++-module std.c++-module\n"
        "CXX_IMPORTS += m.c++-module std.c++-module\n";
    expect(eq(first_record(raw), "obj/main.o: src/main.cpp gcm.cache/std.gcm gcm.cache/m.gcm\n"));
}};

// A plain depfile (clang's shape, or GCC's for a unit with no module) is
// returned whole, including a final line without a newline.
UTest a_plain_depfile_is_kept_whole{"Depfile/APlainDepfileIsKeptWhole", [] {
    const std::string raw = "obj/a.o: src/a.cpp \\\n src/a.h \\\n src/b.h";
    expect(eq(first_record(raw), raw));
    expect(eq(first_record(""), ""));
}};

// Windows paths as a Windows GCC writes them: a drive letter and escaped
// spaces do not end the record; only a line that starts with a non-space does.
UTest windows_paths_and_crlf_lines_stay_in_the_record{"Depfile/WindowsPathsAndCrlfLinesStayInTheRecord", [] {
    const std::string raw =
        "obj/a.o: C:/src/a.cpp \\\r\n"
        " C:/Program\\ Files/inc/a.h\r\n"
        "a.c++-module: gcm.cache/a.gcm\r\n";
    expect(eq(first_record(raw), "obj/a.o: C:/src/a.cpp \\\r\n"
              " C:/Program\\ Files/inc/a.h\r\n"));
}};

// ── cl.exe's record (mcpp#970, design 2026-11-19 §20 T1) ─────────────────────
//
// The record is the one cl.exe 19.44 wrote on windows-2022 (probe mcpp#971):
// UTF-8, every path in lower case, the toolchain's and the SDK's headers
// listed with the project's. The executor gives ninja the paths as lines of
// its output (`deps = msvc` with mcpp's prefix).

namespace {

using mcpp::build::depfile::SourceRecord;
using mcpp::build::depfile::inputs_from_source_dependencies;

constexpr std::string_view kRecord = R"({
    "Version": "1.2",
    "Data": {
        "Source": "c:\\p970\\\u4e2d\u6587\u00e9\\b.cpp",
        "ProvidedModule": "",
        "Includes": [
            "c:\\p970\\\u4e2d\u6587\u00e9\\inc dir\\h\u00e9ader.h",
            "c:\\program files\\microsoft visual studio\\2022\\enterprise\\vc\\tools\\msvc\\14.44.35207\\include\\vector",
            "c:\\program files (x86)\\windows kits\\10\\include\\10.0.26100.0\\ucrt\\stdio.h",
            "c:\\p970\\build\\gen\\config.h",
            "c:\\p970\\build\\gen\\with#hash$.h",
            "c:\\users\\o'neil & co\\a;b^c`d.h"
        ],
        "ImportedModules": [],
        "ImportedHeaderUnits": []
    }
})";

// The file system's spelling of a name, by a table: the directories of the
// build, and a name whose letters beyond ASCII differ only in case.
std::string spelled(const std::string& dir, const std::string& name) {
    if (name == "gen") return "Gen";
    if (name == "config.h") return "Config.h";
    if (name == "\xD0\xB4\xD0\xBC") return "\xD0\x94\xD0\x9C";   // дм as Дм
    (void)dir;
    return name;
}

SourceRecord record_of(std::string_view text) {
    return {.record = text, .buildDir = "C:/p970/Build",
            .toolchainDirs = {"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Tools\\MSVC\\14.44.35207\\include\\",
                              "C:/Program Files (x86)/Windows Kits/10/include/10.0.26100.0/ucrt"},
            .foldCase = true, .spelled = spelled};
}

}  // namespace

// Every path the record names but the toolchain's, as a word of its own: a
// space, `#`, `$`, `'`, `&`, `;`, `^` and a backquote included, which ninja's
// reader of a Make rule could not carry (it ends a name at the last five).
UTest the_record_names_its_files_without_the_toolchains_headers{"Depfile/TheRecordNamesItsFilesWithoutTheToolchainsHeaders", [] {
    const auto inputs = inputs_from_source_dependencies(record_of(kRecord));
    expect(fatal(inputs.has_value())) << (inputs ? std::string() : inputs.error());
    expect(eq(*inputs, std::vector<std::string>{
        "c:/p970/\xE4\xB8\xAD\xE6\x96\x87\xC3\xA9/inc dir/h\xC3\xA9" "ader.h",
        "C:/p970/Build/Gen/Config.h",
        "C:/p970/Build/Gen/with#hash$.h",
        "c:/users/o'neil & co/a;b^c`d.h"}));
}};

// A header the build generated is another edge's output: its node is the build
// directory's spelling and the file's own, not the record's lower case.
UTest a_header_under_the_build_directory_keeps_its_spelling{"Depfile/AHeaderUnderTheBuildDirectoryKeepsItsSpelling", [] {
    auto in = record_of(kRecord);
    in.foldCase = false;  // a host whose names keep their case: the build directory is not matched
    const auto inputs = inputs_from_source_dependencies(in);
    expect(fatal(inputs.has_value()));
    expect(std::ranges::contains(*inputs, std::string("c:/p970/build/gen/config.h")));
}};

// A build directory whose name differs from the record's beyond ASCII in case
// alone (the record spells every path in lower case) is found by the names as
// the file system spells them.
UTest a_build_directory_with_letters_beyond_ascii_is_found{"Depfile/ABuildDirectoryWithLettersBeyondAsciiIsFound", [] {
    auto in = record_of(R"({"Data": {"Includes": ["c:\\\u0434\u043c\\build\\gen\\config.h"]}})");
    in.buildDir = "C:/\xD0\x94\xD0\x9C/Build";
    const auto inputs = inputs_from_source_dependencies(in);
    expect(fatal(inputs.has_value())) << (inputs ? std::string() : inputs.error());
    expect(eq(*inputs, std::vector<std::string>{"C:/\xD0\x94\xD0\x9C/Build/Gen/Config.h"}));
}};

UTest a_record_that_is_not_one_is_refused{"Depfile/ARecordThatIsNotOneIsRefused", [] {
    expect(!inputs_from_source_dependencies(record_of("not json")).has_value());
    expect(!inputs_from_source_dependencies(record_of(R"({"Version": "1.2"})")).has_value());
    expect(!inputs_from_source_dependencies(record_of(R"({"Data": {"Includes": [1]}})")).has_value());
    const auto none = inputs_from_source_dependencies(record_of(R"({"Data": {"Includes": []}})"));
    expect(fatal(none.has_value()));
    expect(none->empty()) << "a unit that includes nothing names nothing";
}};
