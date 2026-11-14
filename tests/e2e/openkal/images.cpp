// Ok images: built once and run by okrun, entered at one stated symbol, the host graph's build
// programs as images, an image of another architecture through its execution host, an image
// that waits for a prepare action, an image on the system that built it, and `cache gc` asking
// the runner (formerly tests/e2e/942, 947, 953, 960, 996, 1009, 1033).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `[ -x <file> ]`
bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

#if defined(__x86_64__) || defined(_M_X64)
constexpr std::string_view host_arch = "x86_64", other_arch = "aarch64";
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view host_arch = "aarch64", other_arch = "x86_64";
#else
constexpr std::string_view host_arch = "", other_arch = "";
#endif

// The script's `case "$(uname -m)"`: a machine with no ok-image row skips.
void needs_image_row() {
    if (host_arch.empty()) skip("no ok-image row for this machine");
}

std::uint32_t machine_of(std::string_view arch) { return arch == "x86_64" ? s::elf_machine::x86_64 : s::elf_machine::aarch64; }

// The first of `<root>/<pattern>` (`ls <pattern> | head -1`).
std::optional<fs::path> first(const fs::path& root, std::string_view pattern) {
    auto found = s::glob(root, pattern);
    if (found.empty()) return std::nullopt;
    return found.front();
}

// `env -i HOME="$HOME" PATH=/usr/bin:/bin <program> <args>`: the program with nothing installed.
e2e::outcome run_bare(const fs::path& program, std::vector<std::string> args) {
    e2e::command c{.argv = {s::on_path("env").value_or("/usr/bin/env").string(), "-i", "HOME=" + s::env("HOME").value_or(""), "PATH=/usr/bin:/bin", program.string()}};
    for (auto& a : args) c.argv.push_back(std::move(a));
    return e2e::run(c);
}

// An ELF64 (little-endian) file's entry point and the flags of its section `name`, read here.
std::uint64_t le(std::string_view b, std::size_t at, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n && at + i < b.size(); ++i) v |= std::uint64_t(static_cast<unsigned char>(b[at + i])) << (8 * i);
    return v;
}
std::uint64_t elf_entry(const fs::path& file) { return le(s::read_file(file), 24, 8); }
std::optional<std::uint64_t> section_flags(const fs::path& file, std::string_view name) {
    const auto b = s::read_file(file);
    if (b.size() < 64 || !b.starts_with("\x7f" "ELF") || b[4] != 2) return std::nullopt;
    const auto shoff = le(b, 0x28, 8), shentsize = le(b, 0x3a, 2), shnum = le(b, 0x3c, 2), shstrndx = le(b, 0x3e, 2);
    const auto strtab = le(b, shoff + shstrndx * shentsize + 0x18, 8);
    for (std::uint64_t i = 0; i < shnum; ++i) {
        const auto sh = shoff + i * shentsize;
        const auto at = strtab + le(b, sh, 4);
        if (at >= b.size()) continue;
        if (std::string_view(b.data() + at) == name) return le(b, sh + 8, 8);
    }
    return std::nullopt;
}

// The address of `symbol` as the llvm payload's `llvm-nm` lists it.
std::optional<std::uint64_t> address_of(const fs::path& nm, const fs::path& image, std::string_view symbol) {
    std::istringstream in{s::run(nm, {image.string()}).out};
    for (std::string line; std::getline(in, line);) {
        std::istringstream words{line};
        std::string value, kind, name;
        if (words >> value >> kind >> name && name == symbol) return std::stoull(value, nullptr, 16);
    }
    return std::nullopt;
}

// No C library, no C++ run-time, no compiler run-time: the image is its own.
constexpr std::string_view no_layers = "[target.'cfg(platform = \"openkal\")'.layers]\nc-abi            = false\n\"c++-abi\"        = false\ncompiler-runtime = false\n";

// An image with no C library: it calls kal_* and supplies the memory routines, stack guard and
// emulated thread-local storage the platform's compile convention requires (SPEC 12.2).
constexpr std::string_view image_main_c = R"(/* An image with no C library: it calls kal_* and supplies the memory routines,
 * stack guard and emulated thread-local storage the platform's compile
 * convention requires (SPEC 12.2). */
typedef unsigned long kal_uintptr;
typedef long kal_intptr;
struct kal_stream { kal_uintptr h; };
struct kal_task { kal_uintptr h; };
struct kal_stream kal_stdout(void);
kal_intptr kal_stream_write(struct kal_stream, const void*, kal_uintptr);
kal_uintptr kal_env_arg_count(void);
kal_uintptr kal_env_var_count(void);
int kal_task_start(void (*entry)(void*), void* arg, struct kal_task* out);
int kal_task_join(struct kal_task);
kal_uintptr kal_task_current(void);
void* kal_alloc(kal_uintptr size, kal_uintptr align);
int kal_random_fill(void*, kal_uintptr);

unsigned long __stack_chk_guard;
__attribute__((noreturn)) void __stack_chk_fail(void) { __builtin_trap(); }

void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* a = d; while (n--) *a++ = (unsigned char)c; return d;
}

struct emutls_control { unsigned long size, align; union { unsigned long index; void* address; } object; void* value; };
static struct { kal_uintptr id; void* vars[8]; } g_ctx[16];
static unsigned long g_next;
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }
void* __emutls_get_address(struct emutls_control* c) {
    lock();
    if (c->object.index == 0) c->object.index = ++g_next;
    const unsigned long index = c->object.index;
    const kal_uintptr self = kal_task_current();
    int slot = -1;
    for (int i = 0; i < 16; ++i) if (g_ctx[i].id == self) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < 16; ++i) if (g_ctx[i].id == 0) { g_ctx[i].id = self; slot = i; break; }
    void** var = &g_ctx[slot].vars[index - 1];
    if (!*var) {
        unsigned char* p = kal_alloc(c->size, c->align ? c->align : 8);
        if (c->value) memcpy(p, c->value, c->size); else memset(p, 0, c->size);
        *var = p;
    }
    void* out = *var;
    unlock();
    return out;
}

static void put(const char* s) { unsigned long n = 0; while (s[n]) ++n; kal_stream_write(kal_stdout(), s, n); }
static __thread long tl = 7;
static void worker(void* arg) { long* seen = arg; *seen = tl; tl = 99; }

__attribute__((noinline)) static int image_main(void) {
    put(kal_env_arg_count() == 3 ? "image: three arguments\n" : "image: arguments lost\n");
    put(kal_env_var_count() > 0 ? "image: an environment\n" : "image: no environment\n");
    tl = 1;
    long seen = 0;
    struct kal_task t;
    if (kal_task_start(worker, &seen, &t) == 0) kal_task_join(t);
    put(seen == 7 && tl == 1 ? "image: each context has its own storage\n" : "image: storage shared\n");
    return 0;
}

/* Seed before entering a protected frame. The image supplies its own C ABI
 * hooks because it deliberately disabled the platform's C library. */
__attribute__((no_stack_protector)) int ok_main(void) {
    if (kal_random_fill(&__stack_chk_guard, sizeof __stack_chk_guard) != 0) return 99;
    return image_main();
}
)";

// A program that throws and prints its argument count, and a test (951, 960).
constexpr std::string_view caught_main = "import std;\nint main(int argc, char** argv) {\n    try { throw std::runtime_error(\"caught\"); }\n    catch (const std::exception& e) { std::println(\"image: {} ({} args)\", e.what(), argc); }\n    return 0;\n}\n";
constexpr std::string_view smoke_test = "import std;\nint main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }\n";

E2E images{"images", [] {
    "942 an ok image is built once and run by okrun"_test[s::llvm][s::python3] = [] {
        // A both rows publish bin/app.oke, an ET_DYN of their architecture, and no unchecked
        // image; B okrun runs this machine's image with its arguments, environment, a callback,
        // TLS; E a second build is up to date; F pack stages the image in its platform's tree,
        // self-contained carries the runner, a single file carries the image; C an image
        // importing `puts` is refused by the seal and not published; D `import std` with the
        // C++ run-time switched off is refused saying where; G a workspace root's image runs.
        needs_image_row();
        const std::string ht = std::string(host_arch) + "-openkal";
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n{}", no_layers)},
                                     {"src/main.c", image_main_c}});
        // A
        for (std::string_view arch : {"x86_64", "aarch64"}) {
            auto r = app.mcpp({"build", "--target", std::format("{}-openkal", arch)});
            expect(fatal(e2e::succeeded(r))) << "A: " << arch << "-openkal did not build: " << s::text(r);
            const auto img = first(app.at("target"), std::format("{}-openkal/*/bin/app.oke", arch));
            expect(fatal(img.has_value())) << "A: " << arch << "-openkal published no bin/app.oke: " << s::text(r);
            const auto h = s::read_header(*img);
            expect(fatal(h.kind == s::format::elf && h.elf_type == 3 && h.machine == machine_of(arch))) << "A: " << arch << "-openkal's image is not an ET_DYN of its architecture";
        }
        expect(fatal(s::glob(app.at("target"), "*-openkal/*/bin/*.unchecked").empty())) << "A: an unchecked image is in bin/";
        // B
        auto r = app.mcpp({"run", "--target", ht, "--", "one", "two"});
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "B: the image did not run through okrun: " << out;
        expect(fatal(e2e::contains(out, "image: three arguments"))) << "B: the image's arguments: " << out;
        expect(fatal(e2e::contains(out, "image: an environment"))) << "B: the image's environment: " << out;
        expect(fatal(e2e::contains(out, "image: each context has its own storage"))) << "B: thread-local storage or the callback: " << out;
        // E
        r = app.mcpp({"build", "--target", ht});
        expect(fatal(e2e::succeeded(r))) << "E: the second build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "E: the second build compiled again: " << s::text(r);
        // F
        r = app.mcpp({"pack", "--target", ht, "--pkg-format", "dir", "-o", "m2"});
        expect(fatal(e2e::succeeded(r))) << "F: pack: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at(std::format("target/dist/m2/lib/{}/app.oke", host_arch))))) << "F: the package has no lib/" << host_arch << "/app.oke: " << s::text(r);
        expect(fatal(executable(app.at("target/dist/m2/bin/app")))) << "F: the package has no launcher: " << s::text(r);
        r = app.mcpp({"pack", "--target", ht, "--mode", "self-contained", "--pkg-format", "dir", "-o", "m3"});
        expect(fatal(e2e::succeeded(r))) << "F: pack --mode self-contained: " << s::text(r);
        auto ran = run_bare(app.at("target/dist/m3/bin/app"), {"one", "two"});
        expect(fatal(e2e::succeeded(ran))) << "F: the self-contained launcher did not run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "image: three arguments"))) << "F: the self-contained package's image: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "image: an environment"))) << "F: the self-contained package's image was given no environment: " << s::text(ran);
        r = app.mcpp({"pack", "--target", ht, "--pkg-format", "single-file", "-o", "m3-file"});
        expect(fatal(e2e::succeeded(r))) << "F: pack --pkg-format single-file: " << s::text(r);
        ran = s::run(app.at("target/dist/m3-file"), {"one", "two"});
        expect(fatal(e2e::succeeded(ran))) << "F: the single file did not run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "image: three arguments"))) << "F: the single file's image: " << s::text(ran);
        // C: reached from the entry (the link discards what nothing reaches)
        const auto published = *first(app.at("target"), ht + "/*/bin/app.oke");
        const auto before = s::read_file(published);
        app.write("src/main.c", "int puts(const char*);\nint ok_main(void) { return puts(\"x\"); }\n");
        r = app.mcpp({"build", "--target", ht});
        out = s::text(r);
        expect(fatal(!e2e::succeeded(r))) << "C: an image importing puts was published: " << out;
        expect(fatal(e2e::contains(out, "puts"))) << "C: the refusal does not name the import: " << out;
        expect(fatal(e2e::contains(out, "was not sealed"))) << "C: the refusal does not name the seal: " << out;
        expect(fatal(s::read_file(*first(app.at("target"), ht + "/*/bin/app.oke")) == before)) << "C: the refused image took the name in bin/: " << out;
        // D
        s::project std_app(tmp / "std", {{"mcpp.toml", "[package]\nname    = \"std-app\"\nversion = \"0.1.0\"\n\n# The package of the C++ run-time is also the compiler run-time's: both off.\n[target.'cfg(platform = \"openkal\")'.layers]\n\"c++-abi\"        = false\ncompiler-runtime = false\n"},
                                         {"src/main.cpp", "import std;\nint main() { std::println(\"x\"); }\n"}});
        r = std_app.mcpp({"build", "--target", ht});
        out = s::text(r);
        expect(fatal(!e2e::succeeded(r))) << "D: import std built without a C++ run-time: " << out;
        expect(fatal(e2e::contains(out, "needs a C++ run-time inside the program"))) << "D: the refusal's reason: " << out;
        expect(fatal(e2e::contains(out, "switched off"))) << "D: the refusal does not say the layer was switched off: " << out;
        expect(fatal(e2e::contains(out, "mcpp.toml"))) << "D: the refusal does not say where: " << out;
        // G
        s::project ws(tmp / "ws", {{"src/main.c", image_main_c},
                                   {"mcpp.toml", std::format("[workspace]\nmembers = [\"m\"]\n\n[package]\nname    = \"wsapp\"\nversion = \"0.1.0\"\n\n[targets.wsapp]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n{}", no_layers)},
                                   {"m/mcpp.toml", "[package]\nname = \"m\"\nversion = \"0.1.0\"\n\n[targets.m]\nkind = \"lib\"\n"},
                                   {"m/src/m.c", "int m_value(void) { return 0; }\n"}});
        r = ws.mcpp({"run", "--target", ht, "--", "one", "two"});
        expect(fatal(e2e::succeeded(r))) << "G: a workspace root's image did not build or run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "image: three arguments"))) << "G: the workspace root's image: " << s::text(r);
    };

    "947 an image is entered at one stated symbol"_test[s::llvm][s::unix_shell] = [] {
        // aarch64-none-elf: A `start = "_reset"` enters the image at _reset over the script's
        // ENTRY(_start), on an Entry line; B a `start` nothing defines fails the link naming the
        // statement; C a board's `mcpp:link-start` enters every image over it; D the program and
        // the board stating different symbols is refused naming both.
        const auto nms = s::glob(s::shared_home() / "registry/data/xpkgs/xim-x-llvm", "*/bin/llvm-nm");
        expect(fatal(!nms.empty())) << "no llvm-nm in the llvm payload";
        const auto nm = nms.front();
        const auto tmp = s::case_dir();
        const auto board = tmp / "board";
        s::project(board, {{"mcpp.toml", "[package]\nname    = \"board\"\nversion = \"0.1.0\"\n\n[targets.board]\nkind = \"lib\"\n"},
                           {"src/board.cppm", "export module board;\nexport namespace board { inline int id() { return 7; } }\n"},
                           {"link.ld", "ENTRY(_start)\nSECTIONS {\n    . = 0x40000000;\n    .text   : { *(.text.entry) *(.text*) }\n    .rodata : { *(.rodata*) }\n    .data   : { *(.data*) }\n    .bss    : { *(.bss*) *(COMMON) }\n    . = ALIGN(16); . = . + 0x4000; __stack_top = .;\n}\n"}});
        const auto board_program = [&](std::string_view statement) {
            s::project(board, {}).write("build.mcpp", std::format("import mcpp;\nimport std;\nint main() {{\n    mcpp::link_script(std::format(\"{{}}/link.ld\", \"{}\").c_str());\n    {}\n    return 0;\n}}\n", board.string(), statement));
        };
        s::project kern(tmp / "kern", {{"src/main.cpp", R"(import board;
extern "C" void kmain() {
    volatile unsigned int* const uart = reinterpret_cast<unsigned int*>(0x09000000);
    *uart = static_cast<unsigned int>(board::id());
    for (;;) { asm volatile("wfi"); }
}
asm(".section .text.entry,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  b _start\n"
    ".globl _reset\n"
    "_reset:\n"
    "  ldr x30, =__stack_top\n"
    "  mov sp, x30\n"
    "  bl kmain\n"
    "1: b 1b\n");
)"}});
        const auto manifest = [&](std::string_view keys) {
            kern.write("mcpp.toml", std::format("[package]\nname    = \"kern\"\nversion = \"0.1.0\"\n\n[build]\ntarget = \"aarch64-none-elf\"\n\n[dependencies]\nboard = {{ path = \"../board\" }}\n\n[targets.kern]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{}\n", keys));
        };
        const auto entered_at_reset = [&] {
            const auto elf = s::find_file(kern.at("target/aarch64-none-elf"), "kern");
            expect(fatal(elf.has_value())) << "no image";
            const auto reset = address_of(nm, *elf, "_reset");
            return reset.has_value() && elf_entry(*elf) == *reset;
        };
        // A
        board_program("");
        manifest(R"(start = "_reset")");
        auto r = kern.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: the image did not build: " << s::text(r);
        expect(fatal(entered_at_reset())) << "A: the image is not entered at _reset";
        expect(fatal(s::has_line(s::text(r), R"(Entry .*kern  _reset  \(\[targets\.kern\] start = "_reset"\))"))) << "A: no Entry line naming the statement: " << s::text(r);
        // B
        manifest(R"(start = "_missing")");
        r = kern.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "B: an image entered at an undefined symbol linked: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "the image is entered at `_missing`, which nothing this program links defines"))) << "B: the note does not name the symbol: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), R"(It was named by [targets.kern] start = "_missing")"))) << "B: the note does not name the statement: " << s::text(r);
        // C
        board_program(R"(mcpp::link_start("_reset");)");
        manifest("");
        r = kern.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: the image over the board did not build: " << s::text(r);
        expect(fatal(entered_at_reset())) << "C: the board's link-start did not enter the image";
        // D
        manifest(R"(start = "_start")");
        r = kern.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r))) << "D: two entry symbols were accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "target 'kern' is entered at one symbol, and the statements disagree"))) << "D: the conflict is not stated: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), R"([targets.kern] start = "_start")") && s::has_line(s::text(r), "mcpp:link-start=_reset. from the build program of")))
            << "D: the conflict does not name both statements: " << s::text(r);
    };

    "953 the host graph runs build programs as images"_test[s::llvm][s::python3] = [] {
        // A `[host] platform = openkal-platform` runs the build program as an image (the
        // report names the host platform) and B its build-dependency's answer arrives; C the
        // host graph's runner is the platform's okrun, a program of this machine; D
        // `--host-platform none` runs it natively with the same answer.
        needs_image_row();
        s::project app({{"mcpp.toml", "[package]\nname    = \"hostgraph\"\nversion = \"0.1.0\"\n\n[host]\nplatform = \"openkal-platform@0.18.0\"\n\n[build-dependencies]\nhelper = { path = \"helper\" }\n"},
                        {"helper/mcpp.toml", "[package]\nname = \"helper\"\nversion = \"0.1.0\"\n\n[targets.helper]\nkind = \"lib\"\n"},
                        {"helper/src/helper.cppm", "export module helper;\nimport std;\nexport namespace helper {\nint answer() { return static_cast<int>(std::string_view(\"forty-two\").size()) * 4 + 6; }\n}\n"},
                        {"build.mcpp", "import mcpp;\nimport std;\nimport helper;\nint main() {\n    mcpp::define(\"BUILT_BY_AN_IMAGE=1\");\n    mcpp::define(std::format(\"HELPER_ANSWER={}\", helper::answer()).c_str());\n    return 0;\n}\n"},
                        {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"host graph: %d, answer %d\\n\", BUILT_BY_AN_IMAGE, HELPER_ANSWER); return 0; }\n"}});
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "A: the project did not build or run: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Host platform +openkal-platform"))) << "A: the report does not name the host platform: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "host graph: 1, answer 42"))) << "A/B: the build program's definitions did not arrive: " << s::text(r);
        r = app.mcpp({"self", "env", "--host", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: self env --host: " << s::text(r);
        const auto d = s::json(r.out)["data"];
        expect(fatal(d["platform"].is_object() && d["platform"]["name"].get<std::string>().ends_with("openkal-platform"))) << "C: " << d["platform"].dump();
        expect(fatal(d["tools"].contains("okrun") && d["tools"]["okrun"].is_string())) << "C: " << d["tools"].dump();
        const auto okrun = d["tools"]["okrun"].get<std::string>();
        expect(fatal(d["runner"].is_object() && d["runner"]["program"] == okrun)) << "C: the host graph's runner: " << d["runner"].dump();
        const auto magic = s::read_file(okrun).substr(0, 4);
        expect(fatal(magic == "\x7f" "ELF" || magic == "\xcf\xfa\xed\xfe" || magic.starts_with("MZ"))) << "C: the runner is no program of this machine";
        expect(fatal(!okrun.ends_with(".oke"))) << "C: the runner is an image: " << okrun;
        fs::remove_all(app.at("target"));
        r = app.mcpp({"run", "--host-platform", "none"});
        expect(fatal(e2e::succeeded(r))) << "D: --host-platform none: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Host platform"))) << "D: a host platform is reported under --host-platform none: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "host graph: 1, answer 42"))) << "D: the answer differs without the host platform: " << s::text(r);
    };

    "960 an image of another architecture runs through its execution host"_test[s::llvm][s::python3] = [] {
        // A the build before a target's tests is recorded under the target; B the other
        // architecture's test is refused once, in full, exit 2; C self env states the refusal,
        // and with an execution host the other architecture's runner; D the execution host is
        // handed that runner (an ELF of that machine) and the image; with qemu it runs.
        needs_image_row();
        const std::string ht = std::string(host_arch) + "-openkal", ot = std::string(other_arch) + "-openkal";
        auto run_under_key = "MCPP_TARGET_" + ot + "_RUN_UNDER";
        for (auto& c : run_under_key) c = c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", caught_main}, {"tests/smoke.cpp", smoke_test}});
        // A
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp run: " << s::text(r);
        r = app.mcpp({"test", "--target", ht});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp test --target " << ht << ": " << s::text(r);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp run after the test: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "the configuration changed"))) << "A: the test's build was recorded as this machine's: " << s::text(r);
        expect(fatal(s::has_line(app.read("target/.build_cache"), std::format(R"(^\[target={}\]$)", ht)))) << "A: no cache entry for " << ht << ": " << app.read("target/.build_cache");
        // B
        r = app.mcpp({"test", "--target", ot});
        auto out = s::text(r);
        expect(fatal(r.started && r.status == 2)) << "B: mcpp test --target " << ot << " exited " << r.status << ", not 2: " << out;
        expect(fatal(e2e::contains(out, "smoke ... not run"))) << "B: the test is not reported not run: " << out;
        expect(fatal(s::count_lines(out, "run_under = ") == 1u)) << "B: the run_under line is not printed exactly once: " << out;
        expect(fatal(e2e::contains(out, std::format("cannot run {} artifacts on this {} host", ot, host_arch)))) << "B: the reason: " << out;
        // C
        r = app.mcpp({"self", "env", "--target", ot});
        expect(fatal(e2e::succeeded(r))) << "C: self env: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "refused on this machine: cannot run " + ot + " artifacts"))) << "C: self env does not say runs are refused: " << s::text(r);
        r = app.mcpp({"self", "env", "--target", ot, "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: self env --format json: " << s::text(r);
        auto runner = s::json(r.out)["data"]["runner"];
        expect(fatal(runner["refusal"].is_string() && e2e::contains(runner["refusal"].get<std::string>(), "run_under"))) << "C: the refusal in JSON: " << runner.dump();
        expect(fatal(runner["arch"] == std::string(host_arch))) << "C: " << runner.dump();
        const auto via = tmp / "via";
        s::write_executable(via, "#!/bin/sh\nprintf \"%s\\n\" \"$@\" > \"$VIA_LOG\"\n[ -n \"$VIA_QEMU\" ] && exec \"$VIA_QEMU\" \"$@\"\nexit 0\n");
        r = app.mcpp({"self", "env", "--target", ot, "--format", "json"}, {.env = {{run_under_key, via.string()}}});
        expect(fatal(e2e::succeeded(r))) << "C: self env with an execution host: " << s::text(r);
        const auto d = s::json(r.out)["data"];
        runner = d["runner"];
        expect(fatal(runner["refusal"].is_null())) << "C: " << runner.dump();
        expect(fatal(runner["arch"] == std::string(other_arch))) << "C: " << runner.dump();
        auto program = runner["program"].get<std::string>();
        std::ranges::replace(program, '\\', '/');
        expect(fatal(e2e::contains(program, "/carrier-pack/okrun-") && program.ends_with(std::string(other_arch) + "/target/dist/packed/bin/okrun"))) << "C: the runner's program: " << program;
        expect(fatal(d["runUnderProgram"].is_string() && fs::equivalent(d["runUnderProgram"].get<std::string>(), via))) << "C: " << d["runUnderProgram"].dump();
        // D
        app.write("mcpp.toml", app.read("mcpp.toml") + std::format("\n[target.{}]\nrun_under = [\"{}\"]\n", ot, via.generic_string()));
        r = app.mcpp({"run", "--target", ot, "--", "one"}, {.env = {{"VIA_LOG", (tmp / "via.run").string()}}});
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "D: mcpp run through the execution host: " << out;
        expect(fatal(e2e::contains(out, std::format("okrun for linux-{}", other_arch)) || e2e::contains(out, std::format("okrun for macos-{}", other_arch)))) << "D: the other architecture's runner was not built: " << out;
        const auto handed = s::read_file(tmp / "via.run");
        const auto runner_path = handed.substr(0, handed.find('\n'));
        expect(fatal(fs::is_regular_file(runner_path))) << "D: the execution host was handed no program: " << handed;
        const auto rh = s::read_header(runner_path);
        expect(fatal(rh.kind == s::format::elf && rh.machine == machine_of(other_arch))) << "D: the runner handed over is machine " << rh.machine << ", not " << other_arch;
        expect(fatal(s::has_line(handed, R"(/bin/app\.oke$)"))) << "D: the image is not the runner's argument: " << handed;
        r = app.mcpp({"test", "--target", ot}, {.env = {{"VIA_LOG", (tmp / "via.test").string()}}});
        expect(fatal(e2e::succeeded(r))) << "D: mcpp test through the execution host: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "smoke ... ok"))) << "D: the test did not pass through the execution host: " << s::text(r);
        const auto tested = s::read_file(tmp / "via.test");
        expect(fatal(tested.substr(0, tested.find('\n')) == runner_path)) << "D: the test's runner is not the run's: " << tested;
        auto qemu = s::on_path(std::format("qemu-{}", other_arch));
        if (!qemu) qemu = s::on_path(std::format("qemu-{}-static", other_arch));
        if (qemu) {
            r = app.mcpp({"run", "--target", ot, "--", "one"}, {.env = {{"VIA_LOG", (tmp / "via.q").string()}, {"VIA_QEMU", qemu->string()}}});
            expect(fatal(e2e::succeeded(r))) << "D: mcpp run under " << *qemu << ": " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "image: caught (2 args)"))) << "D: the image's output under " << *qemu << ": " << s::text(r);
            r = app.mcpp({"test", "--target", ot}, {.env = {{"VIA_LOG", (tmp / "via.qt").string()}, {"VIA_QEMU", qemu->string()}}});
            expect(fatal(e2e::succeeded(r))) << "D: mcpp test under " << *qemu << ": " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "smoke ... ok"))) << "D: the test under " << *qemu << ": " << s::text(r);
        }
    };

    "996 an ok image waits for a prepare action"_test[s::llvm] = [] {
        // A an image whose build.mcpp declares a prepare action builds and runs (the ordering
        // guard does not refuse it); B its link edge waits for the action phony; C its debug
        // information is compressed by the link and its unit's is beside the object (.dwo).
        needs_image_row();
        const std::string t = std::string(host_arch) + "-openkal";
        s::project app({{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                        {"src/main.cpp", "int main() { return 0; }\n"},
                        {"build.mcpp", R"(#include <string>
import mcpp;
int main() {
    const std::string dir = std::string(mcpp::out_dir()) + "/gen";
    mcpp::action a;
    a.id = "gen:dir";
    a.role = mcpp::roles::prepare;
    a.arg("sh").arg("-c").arg(("mkdir -p " + dir + " && : > " + dir + "/made && : > " + dir + ".stamp").c_str())
     .output((dir + ".stamp").c_str())
     .output_dir(dir.c_str())
     .submit();
    return 0;
}
)"}});
        auto r = app.mcpp({"run", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "A: the image did not build and run: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "internal:"))) << "A: the ordering guard refused the build: " << s::text(r);
        std::optional<fs::path> manifest;
        for (const auto& f : s::find_files(app.at("target/" + t), R"(build\.ninja)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { manifest = f; break; }
        expect(fatal(manifest.has_value())) << "B: no build.ninja";
        expect(fatal(s::has_line(s::read_file(*manifest), R"(^build obj/platform/bin/app\.oke\.unsealed[ :].*\|\|.*mcpp-actions-app)"))) << "B: the link edge does not wait for the action: " << *manifest;
        const auto image = s::find_file(app.at("target/" + t), "app.oke");
        expect(fatal(image.has_value())) << "C: no image";
        // `readelf -S -W | grep .debug_info | grep ' C '`: SHF_COMPRESSED (0x800).
        const auto flags = section_flags(*image, ".debug_info");
        expect(fatal(flags.has_value() && (*flags & 0x800) != 0)) << "C: the image's debug information is not compressed";
        expect(fatal(!s::find_files(app.at("target/" + t), R"(main.*\.dwo)").empty())) << "C: the image's unit has no .dwo";
    };

    "1009 cache gc asks the runner"_test[s::unix_shell] = [] {
        // A home with no okrun asks no runner; of the openkal-tools entries in a home, the
        // newest by numeric version (not one that names no released version) is asked
        // `cache gc`, and what it says is reported.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::fresh);
        const s::options fresh{.where = s::home::fresh};
        fs::create_directories(home / "build-cache/v1/tool");
        auto r = s::mcpp({"cache", "gc", "--older-than", "3650d"}, tmp, fresh);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Reclaimed"))) << "a home without okrun reported a runner's cache: " << s::text(r);
        const auto runner = [&](std::string_view version, std::string_view key, std::string_view says) {
            s::write_executable(home / "build-cache/v1/tool/mcpplibs" / std::format("openkal-tools@{}", version) / key / "bin/okrun",
                                std::format("#!/bin/sh\necho \"$@\" > \"{}\"\necho \"{}\" >&2\n", (tmp / std::format("asked-{}", version)).string(), says));
        };
        runner("0.3.2", "aaaa", "okrun: the older runner");
        runner("0.4.1", "bbbb", "okrun: removed 7 bytes prepared for another boot");
        runner("0.4.0", "cccc", "okrun: the middle runner");
        runner("0.10.1", "dddd", "okrun: removed 9 bytes prepared for another boot");
        runner("0.99broken", "eeee", "okrun: not a version");
        runner("999999999999999999999999", "ffff", "okrun: overflowing version");
        r = s::mcpp({"cache", "gc", "--older-than", "3650d"}, tmp, fresh);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto asked = [&](std::string_view v) { return tmp / std::format("asked-{}", v); };
        expect(fatal(fs::exists(asked("0.10.1")) && s::read_file(asked("0.10.1")) == "cache gc\n")) << "the newest okrun was not asked `cache gc`: " << s::text(r);
        expect(fatal(!fs::exists(asked("0.3.2")) && !fs::exists(asked("0.4.0")) && !fs::exists(asked("0.4.1")))) << "an older okrun was asked: " << s::text(r);
        expect(fatal(!fs::exists(asked("0.99broken")) && !fs::exists(asked("999999999999999999999999")))) << "an invalid version was asked: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Reclaimed.*removed 9 bytes prepared for another boot"))) << "what the runner reclaimed is not reported: " << s::text(r);
    };

    "1033 an ok image runs on the system that built it"_test = [] {
        // A this architecture's image runs through okrun with its arguments and environment,
        // std::format and a std::thread; B a test image runs; C the other architecture's row
        // builds an image (without the platform's layers).
        needs_image_row();
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", R"cpp(import std;

int main(int argc, char** argv) {
    std::string args;
    for (int i = 1; i < argc; ++i) args += std::format("[{}]", argv[i]);
    const char* v = std::getenv("MCPP_E2E_1033");
    int sum = 0;
    std::thread t([&] { for (int i = 1; i <= 10; ++i) sum += i; });
    t.join();
    std::println("args={} env={} sum={}", args, v ? v : "(none)", sum);
    return sum == 55 ? 0 : 1;
}
)cpp"},
                                     {"tests/smoke.cpp", "import std;\nint main() { return std::format(\"{}-{}\", 1, 2) == \"1-2\" ? 0 : 1; }\n"}});
        const std::string ht = std::string(host_arch) + "-openkal";
        auto r = app.mcpp({"run", "--target", ht, "--", "one", "two words"}, {.env = {{"MCPP_E2E_1033", "here"}}});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp run --target " << ht << ": " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "args=[one][two words] env=here sum=55"))) << "A: the image's output: " << s::text(r);
        r = app.mcpp({"test", "--target", ht});
        expect(fatal(e2e::succeeded(r))) << "B: mcpp test --target " << ht << ": " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "smoke ... ok"))) << "B: the test image did not pass: " << s::text(r);
        s::project other(tmp / "other", {{"mcpp.toml", std::format("[package]\nname    = \"other\"\nversion = \"0.1.0\"\n\n[targets.other]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n{}", no_layers)},
                                         {"src/main.c", "int main(void) { return 0; }\n"}});
        r = other.mcpp({"build", "--target", std::string(other_arch) + "-openkal"});
        expect(fatal(e2e::succeeded(r))) << "C: mcpp build --target " << other_arch << "-openkal: " << s::text(r);
        expect(fatal(!s::glob(other.at("target"), std::format("{}-openkal/*/bin/other.oke", other_arch)).empty())) << "C: no image for " << other_arch << "-openkal: " << s::text(r);
    };
}};

}  // namespace
