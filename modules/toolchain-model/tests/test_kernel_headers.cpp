import std;
import mcpplibs.mtest;
import mcpp.toolchain.base;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.target_schema;

using namespace mcpplibs::mtest;

// THE KERNEL HEADERS' VERSION AND THE SYSTEM DATA THAT SAYS HOW TO READ IT
// (design 2026-11-22 §3.4). The decoding is the engine's; the macro names and
// the widths are the system row's, so these tests read them from the table.

namespace {
namespace tb = mcpp::toolchain::base;

std::map<std::string, std::string, std::less<>> defines(std::string_view text) {
    return tb::parse_defines(text);
}

const mcpp::toolchain::target::System& linux_system() {
    auto const* s = mcpp::toolchain::target::find_system("linux");
    if (!s) throw std::runtime_error("no linux system row");
    return *s;
}

std::optional<tb::KernelVersion> read(std::string_view text) {
    auto const& k = linux_system().kernelAbi;
    return tb::kernel_version(defines(text), k.version, k.packed, k.bits);
}
} // namespace

UTest the_linux_row_states_its_kernel_headers{"KernelHeaders/TheLinuxRowStatesItsKernelHeaders", [] {
    auto const& k = linux_system().kernelAbi;
    expect(k.stated());
    expect(eq(std::string(k.header), std::string("linux/version.h")));
    expect(eq(k.version.size(), std::size_t{3}));
    expect(eq(k.bits.size(), std::size_t{3}));
    expect(eq(std::string(k.package), std::string("linux-headers")));
    expect(std::string(k.archInclude).find("{arch}") != std::string::npos);
}};

UTest a_system_whose_kernel_is_its_base_states_none{"KernelHeaders/ASystemWhoseKernelIsItsBaseStatesNone", [] {
    // macOS, Windows and FreeBSD publish the kernel interface with their C
    // library: their rows accept no prebuilt kernel-abi statement.
    for (auto os : { "macos", "windows", "freebsd" }) {
        auto const* s = mcpp::toolchain::target::find_system(os);
        expect(s != nullptr);
        if (s) expect(!s->kernelAbi.stated());
    }
}};

UTest the_parts_macros_answer_first{"KernelHeaders/ThePartsMacrosAnswerFirst", [] {
    // linux 6.18.64's <linux/version.h>, as `-E -dM` prints it.
    auto v = read("#define LINUX_VERSION_CODE 397888\n#define LINUX_VERSION_MAJOR 6\n"
                  "#define LINUX_VERSION_PATCHLEVEL 18\n#define LINUX_VERSION_SUBLEVEL 64\n");
    expect(v.has_value());
    if (v) {
        expect(eq(v->text(), std::string("6.18.64")));
        expect(!v->saturated[2]);
    }
}};

UTest the_packed_macro_is_decoded_by_the_rows_widths{"KernelHeaders/ThePackedMacroIsDecodedByTheRowsWidths", [] {
    // linux 5.11.1 defines LINUX_VERSION_CODE alone.
    auto v = read("#define LINUX_VERSION_CODE 330497\n");
    expect(v.has_value());
    if (v) expect(eq(v->text(), std::string("5.11.1")));
    auto packed = read("#define LINUX_VERSION_CODE 397888\n");
    expect(packed.has_value());
    if (packed) expect(eq(packed->text(), std::string("6.18.64")));
}};

UTest a_part_at_its_widths_maximum_is_saturated{"KernelHeaders/APartAtItsWidthsMaximumIsSaturated", [] {
    // linux 4.19.256 packs its sublevel as 255: the packing says no more, and a
    // comparison skips the part.
    auto v = read("#define LINUX_VERSION_CODE 267263\n");
    expect(v.has_value());
    if (v) {
        expect(eq(v->text(), std::string("4.19.255")));
        expect(v->saturated[2]);
        expect(tb::kernel_version_matches(*v, "4.19.256"));
        expect(!tb::kernel_version_matches(*v, "4.20.1"));
    }
}};

UTest a_statement_is_compared_to_the_measurements_precision{"KernelHeaders/AStatementIsComparedToTheMeasurementsPrecision", [] {
    auto v = read("#define LINUX_VERSION_CODE 397888\n");
    expect(v.has_value());
    if (!v) return;
    expect(tb::kernel_version_matches(*v, "6.18.64"));
    expect(!tb::kernel_version_matches(*v, "6.18.63"));
    expect(!tb::kernel_version_matches(*v, "7.3.5"));
    expect(!tb::kernel_version_matches(*v, ""));
}};

UTest no_header_no_version{"KernelHeaders/NoHeaderNoVersion", [] {
    // The probe unit includes the header under __has_include: without it,
    // nothing is defined and nothing is measured.
    expect(!read("#define __linux__ 1\n").has_value());
}};

UTest the_interfaces_are_the_systems_and_the_rows{"KernelHeaders/TheInterfacesAreTheSystemsAndTheRows", [] {
    auto const& l = linux_system();
    expect(l.interfacesStated);
    expect(std::ranges::find(l.interfaces, std::string_view("openkal.process")) != l.interfaces.end());
    // An iOS application starts no program: its rows narrow the system's list.
    auto const* ios = mcpp::toolchain::target::find_row("aarch64-ios");
    expect(ios != nullptr);
    if (ios) {
        expect(ios->interfacesStated);
        expect(std::ranges::find(ios->interfaces, std::string_view("openkal.process")) == ios->interfaces.end());
        expect(std::ranges::find(ios->interfaces, std::string_view("openkal.fs")) != ios->interfaces.end());
    }
}};
