// mcpp.test.support: what an end-to-end case of mcpp is made of (design 2026-11-12 §5.9).
//
//   E2E build{"build", [] {
//       "a project builds and runs"_test = [] {
//           auto p = support::project({{"mcpp.toml", manifest}, {"src/main.cpp", main_cpp}});
//           auto r = p.mcpp({"build"});
//           expect(e2e::succeeded(r));
//           expect(e2e::contains(p.mcpp({"run"}).out, "hello"));
//       };
//   }};
//
// mcpp is the program under test (`MCPP_BIN_MCPP`, which `mcpp test` gives; else `MCPP`). Its home
// is the shared one (`MCPP_HOME`, else `~/.mcpp`: the toolchains a machine already holds) unless a
// case asks for a fresh one, which is the case's own and removed with it.
export module mcpp.test.support;

import std;
export import mcpplibs.mtest;
export import mcpplibs.mtest.e2e;
export import nlohmann.json;

export namespace mcpp::test::support {

namespace fs = std::filesystem;
namespace e2e = mcpplibs::mtest::e2e;

// ---- the machine -------------------------------------------------------------------------------

bool on_windows() noexcept;
bool on_macos() noexcept;
bool on_linux() noexcept;
// `name` as this system names an executable: `name.exe` on Windows.
std::string exe(std::string_view name);
// The value of the environment variable `name`, if set.
std::optional<std::string> env(std::string_view name);
// The executable `name` as PATH finds it (`name.exe` too on Windows).
std::optional<fs::path> on_path(std::string_view name);

// A capability word of a script's `# requires:` line, as tests/e2e/run_all.sh detects it on this
// machine (the same probes: a program on PATH, a payload in the home, the system). An unknown word
// fails the case, so that a misspelt word cannot turn into a skip on every machine.
bool has(std::string_view capability);
// Ends the case as skipped ("missing capability: <word>") unless this machine has every word.
void needs(std::initializer_list<std::string_view> capabilities);

// The newest version (by `sort -V`) of payload `package` (`xim-x-musl-gcc`) for which
// `<version>/<probe>` (a glob) exists, in the shared home's registry or, failing that, the user's
// xlings (tests/e2e/_toolchain_env.sh).
std::optional<std::string> newest_payload(std::string_view package, std::string_view probe = "");
// tests/e2e/_toolchain_env.sh: the version of a toolchain family the case uses, the variable
// `override` when set, else the newest numeric version installed, else `fallback`.
std::string family_version(std::string_view package, std::string_view override, std::string_view fallback);
inline std::string llvm_version() { return family_version("xim-x-llvm", "MCPP_E2E_LLVM_VERSION", "23.1.3"); }
inline std::string gcc_version() { return family_version("xim-x-gcc", "MCPP_E2E_GCC_VERSION", "16.1.0"); }
inline std::string musl_gcc_version() { return family_version("xim-x-musl-gcc", "MCPP_E2E_MUSL_GCC_VERSION", "15.1.0"); }
inline std::string mingw_cross_version() { return family_version("xim-x-mingw-cross-gcc", "MCPP_E2E_MINGW_CROSS_VERSION", "16.1.0"); }
// The directory of that payload's version.
fs::path payload_root(std::string_view package, std::string_view version);

// The root of the mcpp repository these cases belong to (its examples, its fixtures).
fs::path repository();

// ---- mcpp ----------------------------------------------------------------------------------------

// The mcpp under test.
std::string mcpp_path();

// shared: the machine's home. fresh: an empty home of the case. linked: a home of the case whose
// `registry` is a link to the shared home's (its toolchains, not its build cache or state).
// inherited: tests/e2e/_inherit_toolchain.sh's home, a home of the case holding links to each
// payload of the user's homes, their indices, `subos` and `bin`, and a copy of `config.toml`.
// payloads: inherited without config.toml and subos (MCPP_INHERIT_CONFIG=0 MCPP_INHERIT_SUBOS=0).
enum class home { shared, fresh, linked, inherited, payloads };

// The directory of the case, under the system's temporary directory: made on first use and removed
// once the case has ended. Not under the repository, so that mcpp run in a directory of the case
// with no manifest of its own finds no mcpp.toml above it (the repository's). A case makes its
// files here, not in mtest::temp_dir(), which a runner places under the repository's target/.
fs::path case_dir();

// The home of the case (fresh, linked, inherited): one directory per case, made on first use.
fs::path case_home(home kind = home::fresh);
// A further home of the case, made at each call (a case that moves between homes, as a script
// that exported MCPP_HOME again did); removed with the case's other homes. mcpp runs in it with
// `options::env` setting MCPP_HOME.
fs::path another_home(home kind = home::fresh);
// The shared home: `MCPP_HOME`, else `~/.mcpp`.
fs::path shared_home();

struct options {
    home where = home::shared;
    // Set (a value) or removed (nothing) for the run, after the home.
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::string input;
    std::chrono::milliseconds timeout{std::chrono::minutes{10}};
};

// mcpp with `args`, in `cwd`.
e2e::outcome mcpp(std::vector<std::string> args, const fs::path& cwd, const options& o = {});
// The same command, not run: for an `e2e::session` (a mcpp that runs while the case does more).
e2e::command mcpp_command(std::vector<std::string> args, const fs::path& cwd, const options& o = {});

// Standard output, then standard error: what a shell's `2>&1` would have collected, for a
// message whose stream is not what the case asserts.
std::string text(const e2e::outcome& r);

// ---- a project -----------------------------------------------------------------------------------

using files = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// A directory of the case (`case_dir() / "project"`) holding `files` (relative path, content); the
// directories are made.
class project {
public:
    explicit project(files contents = {});
    // A project under an existing directory (a second project beside the first).
    project(fs::path dir, files contents);

    const fs::path& dir() const noexcept { return dir_; }
    fs::path at(std::string_view relative) const;
    void write(std::string_view relative, std::string_view content) const;
    std::string read(std::string_view relative) const;
    bool exists(std::string_view relative) const;
    void remove(std::string_view relative) const;

    // mcpp in this project (or in its subdirectory `sub`).
    e2e::outcome mcpp(std::vector<std::string> args, const options& o = {}) const;
    e2e::outcome mcpp_in(std::string_view sub, std::vector<std::string> args, const options& o = {}) const;

    // The first regular file under `target/` named `name` (an executable's name is given as
    // `exe(name)`), or nothing.
    std::optional<fs::path> artifact(std::string_view name) const;

    // `mcpp new <name> <args...>` in a directory of the case: the project it made (the case fails
    // when mcpp does).
    static project scaffold(std::string_view name, std::vector<std::string> args = {});

private:
    fs::path dir_;
};

// The first regular file under `root` whose name is `name`.
std::optional<fs::path> find_file(const fs::path& root, std::string_view name);
// Every regular file under `root` whose name matches `pattern` (ECMAScript, the whole name).
std::vector<fs::path> find_files(const fs::path& root, std::string_view pattern);
// The paths under `root` that a shell glob of `/`-separated components names, where `*` in a
// component matches any run of characters and, as in a shell, no leading dot:
// glob(p.at("target"), "*/*/build.ninja").
std::vector<fs::path> glob(const fs::path& root, std::string_view pattern);
// Every path under `root`, relative and sorted (`find <root> | sort`): a tree to compare later.
std::vector<std::string> listing(const fs::path& root);
// The most recently written regular file under `root` named `name` (`ls -t $(find ...) | head -1`).
std::optional<fs::path> newest_file(const fs::path& root, std::string_view name);
// `content` written to `file` (its directories made), executable by its owner (a fake tool).
void write_executable(const fs::path& file, std::string_view content);
// A program the case built or wrote, run in `cwd` (a minute at most).
e2e::outcome run(const fs::path& program, std::vector<std::string> args = {}, const fs::path& cwd = {},
                 std::vector<std::pair<std::string, std::optional<std::string>>> env = {});

// ---- a process held by its pid (POSIX systems) -------------------------------------------------
//
// A program the case starts and holds by its process id: signalled alone (not its group), waited
// for, its standard streams a terminal (a pseudo-terminal), a pipe, a file or nothing. The child
// begins with every signal at its default and none blocked, whatever this program was given.
// Where the system has no such processes (Windows), `start` ends the case as failed.

enum class stream {
    null,      // /dev/null
    pipe,      // the pipe the case reads; standard output and standard error given `pipe` share it
    file,      // a file the case names (truncated)
    terminal,  // the pseudo-terminal
    closed,    // a pipe whose reader has already ended
};

struct spawn {
    std::vector<std::string> argv;  // a program without a `/` is looked up on PATH
    fs::path cwd;
    // Set (a value) or removed (nothing), over this program's environment.
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    stream in = stream::null, out = stream::pipe, err = stream::pipe;
    fs::path out_file, err_file;
    // A new session whose controlling terminal is the pseudo-terminal (the terminal's keys, ^C
    // among them, reach the program's group).
    bool session = false;
    unsigned short rows = 40, cols = 120;
};

// The local modes and the input modes of a terminal (`stty -g`'s words), compared whole.
struct terminal_mode {
    unsigned long iflag = 0, oflag = 0, cflag = 0, lflag = 0;
    std::vector<unsigned char> cc;
    bool echo = false, canonical = false;
    bool operator==(const terminal_mode&) const = default;
};

class process {
public:
    static process start(spawn s);
    process() = default;
    process(process&&) noexcept;
    process& operator=(process&&) noexcept;
    process(const process&) = delete;
    process& operator=(const process&) = delete;
    ~process();  // kills (SIGKILL) a program still running

    int pid() const noexcept { return pid_; }
    // Reads what the terminal and the pipe hold, for up to `wait`.
    void pump(std::chrono::milliseconds wait = std::chrono::milliseconds{100});
    // Pumps until `pattern` (ECMAScript) matches what the terminal or the pipe received with
    // escape sequences removed, or `timeout`; whether it matched.
    bool wait_for(std::string_view pattern, std::chrono::milliseconds timeout);
    const std::string& terminal() const noexcept { return term_; }  // the bytes the terminal received
    const std::string& piped() const noexcept { return piped_; }    // the bytes the pipe received
    void send(std::string_view bytes);                                 // typed on the terminal
    terminal_mode mode() const;                                        // the terminal's present mode
    void signal(int sig) const;
    // The status if the program ended (the shell's: 128 + a signal), nothing while it runs.
    std::optional<int> poll();
    // The signal that ended the program (0: it exited), once it ended.
    int ended_by() const noexcept { return signal_; }
    // Waits for the end, reading the streams, and drains them; kills the program at `timeout`.
    int wait(std::chrono::milliseconds timeout = std::chrono::minutes{10});

private:
    int pid_ = -1, master_ = -1, slave_ = -1, pipe_ = -1;
    std::optional<int> status_;
    int signal_ = 0;
    std::string term_, piped_;
};

// Signals by their POSIX numbers (the same on Linux and macOS).
namespace sig {
inline constexpr int interrupt = 2, kill = 9, pipe = 13, terminate = 15;
}

// `stat -c '%i %Y %s'`: a file's inode, modification second and size (a rewrite changes the
// first or the second even when the size is kept). The inode is 0 where the system has none.
std::string file_identity(const fs::path& file);

// `text` without its escape sequences (`CSI ... final`).
std::string strip_escapes(std::string_view text);

// The processes of this machine (Linux, /proc): their id, name (`comm`), working directory and
// command line (the words joined by spaces).
struct process_entry {
    int pid = 0;
    std::string name;
    fs::path cwd;
    std::string command_line;
};
std::vector<process_entry> processes();

// ---- readings --------------------------------------------------------------------------------------

std::string read_file(const fs::path& file);
// The lines of `text` (or of a file) that contain `needle`.
std::vector<std::string> lines_with(std::string_view text, std::string_view needle);
std::vector<std::string> file_lines_with(const fs::path& file, std::string_view needle);
// A JSON document, or the first line of `text` that is one when `text` holds several.
nlohmann::json json(std::string_view text);
nlohmann::json json_file(const fs::path& file);
// Every line of `text` that is a JSON object (a stream of records).
std::vector<nlohmann::json> json_lines(std::string_view text);

// What a script's `grep` asked, as comparisons whose failure shows the text:
// some line of `text` matches `pattern` (ECMAScript; `^` and `$` are the line's ends: `grep -E`);
mcpplibs::mtest::comparison has_line(std::string_view text, std::string_view pattern);
// no line of `text` matches `pattern` (`! grep -E`);
mcpplibs::mtest::comparison no_line(std::string_view text, std::string_view pattern);
// `text` does not contain `needle`;
mcpplibs::mtest::comparison lacks(std::string_view text, std::string_view needle);
// `text` contains `needle`, ignoring ASCII case (`grep -i`).
mcpplibs::mtest::comparison contains_nocase(std::string_view text, std::string_view needle);
// some line of `text` matches `pattern`, ignoring ASCII case (`grep -iE`).
mcpplibs::mtest::comparison has_line_nocase(std::string_view text, std::string_view pattern);
// The last line of `text` that is not empty (`| tail -1` of a program's output).
std::string last_line(std::string_view text);
// The last line of `text` that starts with `prefix` (`grep '^<prefix>' | tail -1`), or "".
std::string last_starting(std::string_view text, std::string_view prefix);
// The number of lines of `text` that contain `needle` (`grep -c`).
std::size_t count_lines(std::string_view text, std::string_view needle);
// The number of lines of `text` that match `pattern` (`grep -cE`).
std::size_t count_matching(std::string_view text, std::string_view pattern);

// The outputs `.ninja_log` records, one per edge run (its fourth column), in order.
std::vector<std::string> ninja_log_outputs(const fs::path& build_dir);

// The words of the compile database `cdb`'s first entry whose `file` (with `\` read as `/`) ends
// with `file_suffix`: its `arguments`, else its `command` split at spaces; nothing when no entry
// does or the file is not a database.
std::optional<std::vector<std::string>> cdb_arguments(const fs::path& cdb, std::string_view file_suffix);

// ---- executables -------------------------------------------------------------------------------------

enum class format { unknown, elf, pe, macho, macho_fat, archive, wasm };

struct header {
    format kind = format::unknown;
    bool wide = false;                 // ELF class 64, PE32+, a 64-bit Mach-O
    std::uint32_t machine = 0;         // e_machine, the COFF machine, cputype
    std::uint16_t subsystem = 0;       // PE: the optional header's subsystem
    std::uint16_t elf_type = 0;        // ET_EXEC, ET_DYN, ...
    bool elf_interpreter = false;      // a PT_INTERP segment: a dynamic program
    std::string interpreter;           // its path
    std::vector<std::string> needed;   // ELF DT_NEEDED
    std::string runpath;               // ELF DT_RUNPATH or DT_RPATH (both, `:`-joined, when both)
    bool elf_rpath = false;            // a DT_RPATH entry
    bool elf_runpath = false;          // a DT_RUNPATH entry
    std::string soname;                // ELF DT_SONAME
};

// The header of an executable, an object or a library, read by this process (no `file`,
// `readelf` or `llvm-readobj`).
header read_header(const fs::path& file);

// The members of a static library (`ar t`) and the symbols its index names (`nm` of the
// archive's defined globals), read by this process: System V/GNU and BSD archives.
std::vector<std::string> archive_members(const fs::path& file);
std::vector<std::string> archive_symbols(const fs::path& file);

// The sections of an ELF file, by name and type (SHT_SYMTAB is 2), read by this process.
struct elf_section {
    std::string name;
    std::uint32_t type = 0;
};
std::vector<elf_section> elf_sections(const fs::path& file);
// The names an ELF symbol table (".symtab", or ".dynsym") holds (`nm`, `nm -D`); with
// `defined_only`, those a section of the file defines (`nm --defined-only`).
std::vector<std::string> elf_symbols(const fs::path& file, std::string_view table = ".symtab", bool defined_only = false);

// An archive's members (`tar -tf`) and their extraction (`tar -xf <archive> -C <into> [members]`),
// by the machine's `tar`, which reads every compression it writes.
std::vector<std::string> tar_list(const fs::path& archive);
void tar_extract(const fs::path& archive, const fs::path& into, std::vector<std::string> members = {});
// A zip's entries from its central directory: the name and the compression method (8: deflated).
struct zip_entry {
    std::string name;
    std::uint16_t method = 0;
};
std::vector<zip_entry> zip_entries(const fs::path& file);
// The SHA-256 of `data`, in lower-case hex (`sha256sum`).
std::string sha256(std::string_view data);

namespace elf_machine {
inline constexpr std::uint32_t x86 = 3, arm = 40, x86_64 = 62, aarch64 = 183, riscv = 243;
}
namespace pe_machine {
inline constexpr std::uint32_t i386 = 0x14c, x86_64 = 0x8664, arm64 = 0xaa64;
}

}  // namespace mcpp::test::support
