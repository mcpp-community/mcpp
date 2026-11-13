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
// Whether this build runs x86_64 code (the `x86_64-host` of a script's `# requires:`).
bool on_x86_64() noexcept;
// `name` as this system names an executable: `name.exe` on Windows.
std::string exe(std::string_view name);
// The value of the environment variable `name`, if set.
std::optional<std::string> env(std::string_view name);
// The executable `name` as PATH finds it (`name.exe` too on Windows).
std::optional<fs::path> on_path(std::string_view name);
// A C compiler of the host that links a shared object: `gcc`, then `cc`, as PATH finds them, then
// /usr/bin's. PATH may lead first to a toolchain's wrapper that compiles but finds no C runtime to
// link (a subos `gcc` outside its sysroot). Probed once; nothing when none links.
std::optional<fs::path> host_cc();

// ---- where a case runs, and what it needs of the machine (mtest 0.8.0) --------------------------
//
// A case states both as attributes, "x"_test[s::elf][s::gcc]; the program skips it elsewhere,
// saying why, whichever runner starts it, and the case's tags carry `requires:<word>` for each
// capability. The words are those of a script's `# requires:` line (tests/e2e/run_all.sh).

using mcpplibs::mtest::architecture;
using mcpplibs::mtest::capability;
using mcpplibs::mtest::only;
using mcpplibs::mtest::platform;

// Places: the systems whose conventions a case's subject has.
const only elf{platform::linux};                         // a host whose programs are ELF
const only unix_shell{platform::linux, platform::macos};  // a POSIX shell and its tools
const only fresh_sandbox{platform::linux, platform::macos};
const only symlink{platform::linux, platform::macos};
const only macos{platform::macos};
const only windows{platform::windows};
const only x86_64_host{architecture::x86_64};

// What a payload or the system provides, which no word of the probe vocabulary states (a payload
// of the home's registry or the user's xlings, a device adb answers, a Visual Studio vswhere names).
bool provides(std::string_view word);

// Capabilities: programs, payloads and devices.
const capability gcc{"gcc", {.os = {platform::linux}, .program = {"g++"}}};
const capability patchelf{"patchelf", {.os = {platform::linux}, .program = {"patchelf"}}};
const capability pack{"pack", {.all = {patchelf}}};
const capability python3{"python3", {.program = {"python3"}}};
const capability jq{"jq", {.program = {"jq"}}};
const capability wine{"wine", {.os = {platform::linux}, .program = {"wine"}}};
const capability qemu_riscv{"qemu-riscv", {.os = {platform::linux}, .program = {"qemu-system-riscv64"}}};
const capability xlings_msvc{"xlings-msvc", {.env = {{"MCPP_E2E_XLINGS_MSVC", "1"}}}};
const capability musl{"musl", {.os = {platform::linux}, .test = [] { return provides("musl"); }}};
const capability mingw_cross{"mingw-cross", {.os = {platform::linux}, .test = [] { return provides("mingw-cross"); }}};
const capability mingw_host_headers{"mingw-host-headers",
                                    {.os = {platform::linux}, .test = [] { return provides("mingw-host-headers"); }}};
const capability llvm{"llvm", {.os = {platform::linux}, .test = [] { return provides("llvm"); }}};
const capability qemu_arm{"qemu-arm", {.os = {platform::linux}, .test = [] { return provides("qemu-arm"); }}};
const capability android_ndk{"android-ndk", {.os = {platform::linux}, .test = [] { return provides("android-ndk"); }}};
const capability android_device{"android-device",
                                {.os = {platform::linux}, .test = [] { return provides("android-device"); }}};
const capability msvc{"msvc", {.os = {platform::windows}, .test = [] { return provides("msvc"); }}};
const capability no_msvc{"no-msvc", {.os = {platform::windows}, .none = {msvc}}};
const capability mingw{"mingw", {.os = {platform::windows}, .test = [] { return provides("mingw"); }}};
const capability nasm{"nasm", {.test = [] { return provides("nasm"); }}};
const capability scan_deps{"scan-deps", {.test = [] { return provides("scan-deps"); }}};
const capability import_std_libcxx{"import-std-libcxx", {.test = [] { return provides("import-std-libcxx"); }}};

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

// The shared home's `[toolchain] default`, restored when the object ends however the case ends: a
// case that sets the machine's default (`mcpp toolchain default`) holds one, and resource{"machine"}.
class default_kept {
public:
    default_kept();
    default_kept(const default_kept&) = delete;
    default_kept& operator=(const default_kept&) = delete;
    ~default_kept();
private:
    std::string original_;
};

// The root of the mcpp repository these cases belong to (its examples, its fixtures).
fs::path repository();

// The file time of a system time (`touch -d`, `touch -t`). The standard libraries convert between
// the two clocks differently (libc++ has no clock_cast, MSVC's file_clock no to_sys), so the time
// is carried across by the clocks' present instants, to within the time between the two reads.
template <class Duration>
fs::file_time_type file_time(std::chrono::sys_time<Duration> t) {
    const auto sys_now = std::chrono::system_clock::now();
    const auto file_now = fs::file_time_type::clock::now();
    return file_now + std::chrono::duration_cast<fs::file_time_type::duration>(t - sys_now);
}

// ---- mcpp ----------------------------------------------------------------------------------------

// The mcpp under test: MCPP_E2E_MCPP when it is set (a released mcpp, tested by the cases of an
// archive, which name the package's own build); else the package's program `mcpp test` built and
// names MCPP_BIN_MCPP; else MCPP; else `mcpp` on PATH.
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
// The outcome read as text: on Windows each "\r\n" of its streams becomes "\n", as the POSIX tools of
// a Windows shell read lines; a bare "\r" (a redraw) is kept. `mcpp`, `run` and the archive helpers
// return their outcomes so; a case that calls `e2e::run` itself and reads the streams applies it.
e2e::outcome as_text(e2e::outcome r);

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
    // The file as text: on Windows each "\r\n" is read as "\n" (`as_text`).
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

// ---- a local TCP server (BSD sockets; Winsock on Windows) ---------------------------------------
//
// A server on 127.0.0.1 at a port the system chooses (an oracle a case points mcpp at: an HTTP
// resource that answers, stalls or trickles, a TLS peer that refuses). Each connection is handled
// on a thread of its own by the handler, and closed when the handler returns; the server stops and
// joins its threads when destroyed.

class connection {
public:
    explicit connection(std::intptr_t fd) noexcept : fd_(fd) {}
    // Up to `max` bytes, waiting at most `wait`; "" when the peer closed or nothing came.
    std::string receive(std::size_t max = 4096, std::chrono::milliseconds wait = std::chrono::seconds{2});
    // Bytes received until `end` was seen (an HTTP request's head: "\r\n\r\n"), or the peer closed.
    std::string receive_until(std::string_view end, std::chrono::milliseconds wait = std::chrono::seconds{2});
    // Whether every byte was written (false once the peer has closed).
    bool send(std::string_view bytes);

private:
    std::intptr_t fd_;  // a socket: an int, or a Windows SOCKET
};

class local_server {
public:
    explicit local_server(std::function<void(connection&)> handler);
    local_server(const local_server&) = delete;
    local_server& operator=(const local_server&) = delete;
    ~local_server();
    unsigned short port() const noexcept { return port_; }

private:
    std::intptr_t fd_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    std::function<void(connection&)> handler_;
    std::mutex m_;
    std::vector<std::thread> threads_;
    std::thread acceptor_;
};

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
    std::string rpath;                 // ELF DT_RPATH alone
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
// The entries of that table with what `nm` and `readelf --dyn-syms` print beside a name: whether a
// section of the file defines it (`UND` when not) and its binding (STB_GLOBAL 1, STB_WEAK 2,
// STB_GNU_UNIQUE 10). Nothing when the file has no such table.
struct elf_symbol {
    std::string name;
    bool defined = false;
    std::uint8_t binding = 0;
};
std::vector<elf_symbol> elf_symbol_table(const fs::path& file, std::string_view table = ".symtab");

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
