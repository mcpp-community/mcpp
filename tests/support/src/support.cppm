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

// ---- mcpp ----------------------------------------------------------------------------------------

// The mcpp under test.
std::string mcpp_path();

enum class home { shared, fresh };

struct options {
    home where = home::shared;
    // Set (a value) or removed (nothing) for the run, after the home.
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::string input;
    std::chrono::milliseconds timeout{std::chrono::minutes{10}};
};

// mcpp with `args`, in `cwd`.
e2e::outcome mcpp(std::vector<std::string> args, const fs::path& cwd, const options& o = {});

// Standard output, then standard error: what a shell's `2>&1` would have collected, for a
// message whose stream is not what the case asserts.
std::string text(const e2e::outcome& r);

// ---- a project -----------------------------------------------------------------------------------

using files = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// A directory of the case holding `files` (relative path, content); the directories are made.
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

private:
    fs::path dir_;
};

// The first regular file under `root` whose name is `name`.
std::optional<fs::path> find_file(const fs::path& root, std::string_view name);
// Every regular file under `root` whose name matches `pattern` (ECMAScript, the whole name).
std::vector<fs::path> find_files(const fs::path& root, std::string_view pattern);

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
    std::string runpath;               // ELF DT_RUNPATH or DT_RPATH
};

// The header of an executable, an object or a library, read by this process (no `file`,
// `readelf` or `llvm-readobj`).
header read_header(const fs::path& file);

namespace elf_machine {
inline constexpr std::uint32_t x86 = 3, arm = 40, x86_64 = 62, aarch64 = 183, riscv = 243;
}
namespace pe_machine {
inline constexpr std::uint32_t i386 = 0x14c, x86_64 = 0x8664, arm64 = 0xaa64;
}

}  // namespace mcpp::test::support
