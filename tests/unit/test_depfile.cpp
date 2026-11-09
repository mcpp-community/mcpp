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
