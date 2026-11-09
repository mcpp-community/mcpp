
import std;
import mcpplibs.mtest;
import mcpplibs.os.platform;
import mcpplibs.os.fs;
import mcpp.invocation;
import mcpp.home.files;

using namespace mcpplibs::mtest;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-platform-fs-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path);
    out << text;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}

UTest replace_file_atomically_replaces_existing_file{"PlatformFs/ReplaceFileAtomicallyReplacesExistingFile", [] {
    TempDir temp;
    auto source = temp.path / "new.tmp";
    auto destination = temp.path / "compile_commands.json";
    write_file(source, "new");
    write_file(destination, "old");

    std::error_code ec;
    expect(bool(mcpp::home::replace_file(source, destination, ec)))
        << ec.message();
    expect(!std::filesystem::exists(source));
    expect(eq(read_file(destination), "new"));
}};

UTest replace_failure_keeps_existing_destination{"PlatformFs/ReplaceFailureKeepsExistingDestination", [] {
    TempDir temp;
    auto destination = temp.path / "compile_commands.json";
    write_file(destination, "last-known-good");

    std::error_code ec;
    expect(!mcpp::home::replace_file(
        temp.path / "missing.tmp", destination, ec));
    expect(bool(ec));
    expect(eq(read_file(destination), "last-known-good"));
}};

} // namespace

// ── extended_length (mcpp#641, item 3) ────────────────────────────────────

// Outside Windows there is no path limit of this kind, so the helper is the
// identity: a relative path stays relative and nothing is spelled differently.
UTest extended_length_is_the_identity_outside_windows{"PlatformFs/ExtendedLengthIsTheIdentityOutsideWindows", [] {
    if constexpr (mcpplibs::os::is_windows) {
        skip("the Windows branch is covered by the spelling tests");
    } else {
        const std::filesystem::path rel{"obj/pkg/__pkg/dep/a.cpp.ddi"};
        expect(eq(mcpplibs::os::fs::extended(rel), rel));
        const std::filesystem::path abs{"/tmp/x/../y"};
        expect(eq(mcpplibs::os::fs::extended(abs), abs));
        expect(mcpplibs::os::fs::extended({}).empty());
    }
}};

// The Windows rule, as a pure function, so every host tests it.
UTest windows_extended_length_spells_a_drive_path{"PlatformFs/WindowsExtendedLengthSpellsADrivePath", [] {
    using mcpplibs::os::fs::windows_extended;
    expect(eq(mcpplibs::os::fs::windows_extended("C:/Users/runner/.mcpp/obj/a.ddi"), "\\\\?\\C:\\Users\\runner\\.mcpp\\obj\\a.ddi"));
    // `.` and `..` are resolved lexically, because the prefix turns their
    // processing off; `..` does not climb above the drive.
    expect(eq(mcpplibs::os::fs::windows_extended("C:\\a\\.\\b\\..\\c\\"), "\\\\?\\C:\\a\\c"));
    expect(eq(mcpplibs::os::fs::windows_extended("C:/../../x"), "\\\\?\\C:\\x"));
    expect(eq(mcpplibs::os::fs::windows_extended("D:/"), "\\\\?\\D:\\"));
}};

UTest windows_extended_length_spells_a_unc_path{"PlatformFs/WindowsExtendedLengthSpellsAUncPath", [] {
    using mcpplibs::os::fs::windows_extended;
    expect(eq(mcpplibs::os::fs::windows_extended("//server/share/dir/../f.o"), "\\\\?\\UNC\\server\\share\\f.o"));
    // `..` does not climb above the share.
    expect(eq(mcpplibs::os::fs::windows_extended("\\\\server\\share\\..\\..\\f"), "\\\\?\\UNC\\server\\share\\f"));
}};

UTest windows_extended_length_leaves_prefixed_and_relative_paths_alone{"PlatformFs/WindowsExtendedLengthLeavesPrefixedAndRelativePathsAlone", [] {
    using mcpplibs::os::fs::windows_extended;
    // Idempotent: a path handed on to a child process is converted again there.
    expect(eq(mcpplibs::os::fs::windows_extended("\\\\?\\C:\\a\\b"), "\\\\?\\C:\\a\\b"));
    expect(eq(mcpplibs::os::fs::windows_extended("\\\\.\\pipe\\x"), "\\\\.\\pipe\\x"));
    expect(eq(mcpplibs::os::fs::windows_extended("obj/a.ddi"), "obj/a.ddi"));
    expect(eq(mcpplibs::os::fs::windows_extended(""), ""));
}};

// A NAME THE SHELL ANSWERS FOR ITSELF still resolves to the program on PATH.
// `command -v true` prints `true`, because a shell runs its own builtin, and
// the path is then found by walking PATH (mcpp#755: a bare-name payload
// override of such a name was refused as "not found on PATH" on a machine
// carrying /usr/bin/true).
UTest which_resolves_a_name_that_is_also_a_shell_builtin{"PlatformFs/WhichResolvesANameThatIsAlsoAShellBuiltin", [] {
#if defined(_WIN32)
    skip("`where` reports programs only; the shell answers nothing");
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file("/usr/bin/true", ec)
        && !std::filesystem::is_regular_file("/bin/true", ec))
        skip("this host has no `true` program to find");
    const auto found = mcpp::invocation::which("true");
    expect(fatal(found.has_value()));
    expect(eq(found->filename(), "true"));
    if (auto&& failed = expect(std::filesystem::is_regular_file(*found, ec)); !failed) failed << found->string();
#endif
}};

UTest which_still_answers_nothing_for_a_name_no_host_has{"PlatformFs/WhichStillAnswersNothingForANameNoHostHas", [] {
    expect(!(mcpp::invocation::which("mcpp-no-such-program-7c25d9f6").has_value()));
}};
