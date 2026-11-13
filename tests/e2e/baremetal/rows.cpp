// The bare-metal rows on the zero-libc tier: aarch64-none-elf and x86_64-none-elf produce an image
// whose sections are where the linker script asked, with no undefined symbol, from a project that
// names no C library; x86_64-none-elf links with ld.lld directly and refuses a shared library in
// words; i686-none-elf links ld.lld under elf_i386, or the compiler driver when a platform states
// `link.driver = "compiler"` (formerly tests/e2e/136, 137, 1041).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A tool of the LLVM payload (`llvm-readelf`, `llvm-nm`); the case is skipped where none is installed.
fs::path llvm_tool(std::string_view name) {
    const auto tool = s::payload_root("xim-x-llvm", s::llvm_version()) / "bin" / s::exe(name);
    if (!fs::is_regular_file(tool)) skip(std::format("the LLVM payload has no {} to check with ({})", name, tool.string()));
    return tool;
}

std::string tool_output(const fs::path& tool, std::vector<std::string> args) {
    auto r = s::run(tool, std::move(args));
    expect(fatal(e2e::succeeded(r))) << tool.filename().string() << ": " << s::text(r);
    return r.out;
}

// The manifest of a probe whose row is its only statement: no `[target.*]` section, so the
// zero-libc tier is the row's own property rather than something the project asked for.
std::string probe_manifest(std::string_view name, std::string_view target) {
    return std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[build]\ntarget = \"{}\"\n", name, target);
}

std::string link_script(std::string_view base) {
    return std::format("ENTRY(_start)\nSECTIONS {{\n    . = {};\n    .text   : {{ *(.text.entry) *(.text*) }}\n    .rodata : {{ *(.rodata*) }}\n"
                       "    .data   : {{ *(.data*) }}\n    .bss    : {{ *(.bss*) *(COMMON) }}\n    . = ALIGN(16); . = . + 0x4000; __stack_top = .;\n}}\n", base);
}

constexpr std::string_view build_program = "import mcpp;\nint main() { mcpp::link_script(\"link.ld\"); return 0; }\n";

// The image under target/<triple>, named `name`.
fs::path image_of(const s::project& p, std::string_view triple, std::string_view name) {
    const auto found = s::find_file(p.at("target") / triple, name);
    expect(fatal(found.has_value())) << "no image was produced under target/" << triple;
    return *found;
}

// `.text` starts at `address` (llvm-readelf -S), and nothing is left undefined (llvm-nm -u): a row
// whose sysroot column named a payload that does not exist would add no paths and produce the
// second symptom later, inside whichever package first included a C header.
void placed_and_closed(const fs::path& image, std::string_view address) {
    const auto readelf = llvm_tool("llvm-readelf"), nm = llvm_tool("llvm-nm");
    const auto sections = tool_output(readelf, {"-S", image.string()});
    expect(fatal(s::has_line(sections, std::format(R"(\.text .*{})", address)))) << "the linker script's load address did not reach the image: " << sections;
    const auto undefined = tool_output(nm, {"-u", image.string()});
    expect(fatal(s::count_matching(undefined, R"(\S)") == 0)) << "the image expects symbols nothing provides: " << undefined;
}

E2E rows{"rows", [] {
    "136 aarch64 zero libc target"_test[tag{"requires:llvm"}] = [] {
        // A the row builds with no C library anywhere in the project; B the image is aarch64 and
        // starts where the script says; C nothing is undefined; D the ABI flag is the measured
        // `-mabi=aapcs`, never the `-mabi=lp64` the RISC-V rows' spelling suggests (clang rejects
        // it; a build that had succeeded with it would be the more dangerous result).
        s::needs({"llvm"});
        s::project p{{"mcpp.toml", probe_manifest("a64probe", "aarch64-none-elf")},
                     {"src/main.cpp", "namespace {\nvolatile unsigned int* const kUart = reinterpret_cast<unsigned int*>(0x09000000);\n}\n"
                                      "extern \"C\" void kmain() {\n    for (const char* p = \"a64 ok\\n\"; *p; ++p) *kUart = static_cast<unsigned int>(*p);\n"
                                      "    for (;;) { asm volatile(\"wfi\"); }\n}\n"
                                      "asm(\".section .text.entry,\\\"ax\\\",@progbits\\n\"\n    \".globl _start\\n\"\n    \"_start:\\n\"\n"
                                      "    \"  ldr x30, =__stack_top\\n\"\n    \"  mov sp, x30\\n\"\n    \"  bl kmain\\n\"\n    \"1: b 1b\\n\");\n"},
                     {"link.ld", link_script("0x40000000")},
                     {"build.mcpp", build_program}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: aarch64-none-elf did not build: " << s::text(r);
        const auto image = image_of(p, "aarch64-none-elf", "a64probe");
        const auto h = s::read_header(image);
        expect(fatal(h.kind == s::format::elf && h.wide && h.machine == s::elf_machine::aarch64)) << "B: the image is not aarch64 (machine " << h.machine << ")";
        placed_and_closed(image, "0000000040000000");
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build", "--verbose"});
        const auto out = s::text(r);
        expect(fatal(e2e::contains(out, "-mabi=aapcs"))) << "D: the aarch64 ABI flag is not aapcs: " << out;
        expect(fatal(s::lacks(out, "-mabi=lp64"))) << "D: an ABI clang rejects reached the command line: " << out;
    };

    "137 x86_64 zero libc target"_test[tag{"requires:llvm"}] = [] {
        // clang has no BareMetal toolchain for x86_64, so the row carries an lld emulation and the
        // engine links with ld.lld itself. A it builds; B x86-64, `.text` at 0x100000; C nothing
        // undefined and no PT_INTERP (`--no-dynamic-linker` kept); D the link map exists, so the
        // map flag was rendered for ld.lld; E the link names ld.lld, not the host's g++; F
        // `-march=x86-64` and `-mno-red-zone`; G a shared library is refused in words, not by
        // ninja's "unknown build rule".
        s::needs({"llvm"});
        s::project p{{"mcpp.toml", probe_manifest("x86probe", "x86_64-none-elf")},
                     {"src/main.cpp", "namespace {\ninline void outb(unsigned short port, unsigned char v) {\n    asm volatile(\"outb %0, %1\" :: \"a\"(v), \"Nd\"(port));\n}\n}\n"
                                      "extern \"C\" void kmain() {\n    for (const char* p = \"x86 ok\\n\"; *p; ++p) outb(0x3F8, static_cast<unsigned char>(*p));\n"
                                      "    for (;;) { asm volatile(\"hlt\"); }\n}\n"
                                      "asm(\".section .text.entry,\\\"ax\\\",@progbits\\n\"\n    \".globl _start\\n\"\n    \"_start:\\n\"\n"
                                      "    \"  movq $__stack_top, %rsp\\n\"\n    \"  call kmain\\n\"\n    \"1: hlt\\n\"\n    \"  jmp 1b\\n\");\n"},
                     {"link.ld", link_script("0x100000")},
                     {"build.mcpp", build_program}};
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: x86_64-none-elf did not build: " << s::text(r);
        const auto image = image_of(p, "x86_64-none-elf", "x86probe");
        const auto h = s::read_header(image);
        expect(fatal(h.kind == s::format::elf && h.wide && h.machine == s::elf_machine::x86_64)) << "B: the image is not x86-64 (machine " << h.machine << ")";
        placed_and_closed(image, "0000000000100000");
        expect(fatal(!h.elf_interpreter)) << "C: a bare-metal image names a dynamic loader: " << h.interpreter;
        expect(fatal(fs::is_regular_file(fs::path(image.string() + ".map")))) << "D: no link map beside the image";
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build", "--verbose"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "E: the verbose build: " << out;
        expect(fatal(e2e::contains(out, "ld.lld"))) << "E: the link did not go through ld.lld: " << out;
        expect(fatal(e2e::contains(out, "-march=x86-64"))) << "F: the x86_64 ISA profile is not the measured one: " << out;
        expect(fatal(e2e::contains(out, "-mno-red-zone"))) << "F: the red zone was left enabled on a target that takes interrupts: " << out;
        s::project shr(s::case_dir() / "shared", {{"mcpp.toml", "[package]\nname    = \"shr\"\nversion = \"0.1.0\"\n\n[build]\ntarget = \"x86_64-none-elf\"\n\n[targets.shr]\nkind = \"shared\"\n"},
                                                  {"src/lib.cpp", "int f() { return 1; }\n"}});
        r = shr.mcpp({"build"});
        const auto refused = s::text(r);
        expect(fatal(!e2e::succeeded(r))) << "G: a shared library was built for a target with no loader: " << refused;
        expect(fatal(e2e::contains(refused, "no dynamic loader"))) << "G: the refusal does not name the situation: " << refused;
        expect(fatal(s::lacks(refused, "unknown build rule"))) << "G: the failure names an internal ninja rule instead of the cause: " << refused;
    };

    "1041 a 32 bit x86 bare target"_test[tag{"requires:elf"}][tag{"requires:llvm"}] = [] {
        // mcpp#911 G12, G4; SPEC-016 §3. The artifact is an ELF32 Intel 80386 executable; the
        // link line is ld.lld's (`-m elf_i386`) without a platform and the compiler's
        // (`--target=i686-none-elf ... -fuse-ld=`) with one stating `link.driver = "compiler"`;
        // where the machine runs 32-bit programs, both artifacts exit 7.
        s::needs({"elf", "llvm"});
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"boot32\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.boot32]\nkind = \"bin\"\nmain = \"src/main.c\"\n"},
                                     {"src/main.c", "#if !defined(__i386__) || defined(__x86_64__)\n#error \"not a 32-bit x86 compilation\"\n#endif\n"
                                                    "_Static_assert(sizeof(void*) == 4, \"a 32-bit pointer\");\n"
                                                    "void _start(void) {\n    __asm__ volatile(\"mov $1, %eax\\n mov $7, %ebx\\n int $0x80\");\n}\n"}});
        const auto check = [&](std::string_view what, std::string_view link_line) {
            const auto exe = s::find_file(app.at("target/i686-none-elf"), "boot32");
            expect(fatal(exe.has_value())) << what << ": no artifact under target/i686-none-elf";
            const auto h = s::read_header(*exe);
            expect(fatal(h.kind == s::format::elf && !h.wide)) << what << ": the artifact is not ELF32";
            expect(fatal(h.machine == s::elf_machine::x86)) << what << ": the artifact is not for i386 (machine " << h.machine << ")";
            const auto ninja = s::find_file(app.at("target/i686-none-elf"), "build.ninja");
            expect(fatal(ninja.has_value())) << what << ": no build.ninja";
            const auto text = s::read_file(*ninja);
            expect(fatal(s::has_line(text, std::format("^c_ldflags += {}", link_line)))) << what << " does not link: " << text;
#if defined(__x86_64__) && defined(__linux__)
            // A 32-bit program runs here when the kernel has IA-32 emulation.
            auto ran = s::run(*exe);
            if (ran.started && ran.status != 126)
                expect(fatal(ran.status == 7)) << what << ": the artifact exited " << ran.status << ", not 7";
#endif
        };
        auto r = app.mcpp({"build", "--target", "i686-none-elf"});
        expect(fatal(e2e::succeeded(r))) << "i686-none-elf did not build: " << s::text(r);
        check("ld.lld under elf_i386", "-m elf_i386");
        s::project bsp(tmp / "bsp", {{"mcpp.toml", std::format("[package]\nname    = \"bsp32\"\nversion = \"0.1.0\"\n\n[platform]\ntargets   = [\"i686-none-elf\"]\n"
                                                                 "toolchain = \"llvm@{}\"\n\n[platform.link]\ndriver = \"compiler\"\n", s::llvm_version())}});
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[target.i686-none-elf]\nplatform = { path = \"../bsp\" }\n");
        r = app.mcpp({"build", "--target", "i686-none-elf"});
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "i686-none-elf with the platform did not build: " << out;
        expect(fatal(e2e::contains(out, "Platform bsp32 0.1.0"))) << "the platform is not reported: " << out;
        check("the platform's compiler driver", ".*--target=i686-none-elf .*-fuse-ld=");
    };
}};

}  // namespace
