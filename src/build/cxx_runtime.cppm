// mcpp.build.cxx_runtime — one C++ runtime per process, and the islands that
// are allowed to differ (design 2026-10-26 §6, D9–D13; SPEC-002 §6).
//
// TWO ITANIUM C++ RUNTIMES SHARE THEIR ABI NAMES. `__cxa_throw`, the
// personality routine, `operator new` and the `std::exception` family outside
// libc++'s `__1` namespace are spelled the same by libstdc++ and by
// libc++/libc++abi. In one process's global symbol space the two either split
// the ABI layer between them -- mcpp#838: the throw from one runtime, the
// catch from the other, and `catch (const std::exception&)` missed -- or
// interpose each other (mcpp#596). `std::runtime_error` and its relatives
// even have DIFFERENT LAYOUTS (a COW string in libstdc++, a refstring in
// libc++), so constructing one with one runtime and destroying it with the
// other is memory corruption. No link order makes "several images, two
// runtimes, exceptions crossing" reliable.
//
// So a build has ONE C++ runtime (U, the default), chosen through the target
// side; and a package that needs another declares it privately
// (`requires = [{ layer = "mcpp:c++-abi=<impl>", visibility = "private" }]`)
// and becomes an ISLAND, accepted only where the engine verifies:
//
//   1. one C++ image of ours (no C++ shared library in the build);
//   2. one unwinder (`--unwindlib=libgcc`, the runtime table);
//   3. the build's own runtime before the other on the link line;
//   4. island objects reference no layout-dependent member of the collision set;
//   5. the image imports no ABI-layer name at the other runtime's versions;
//   6. no typeinfo crosses: the image imports none from an image built on the
//      other runtime (only `catch (...)` could catch its exceptions).
//
// 1 and 3 are decided when the build is planned, 2 by the runtime table, and
// 4–6 here, after the link, from the objects' and the image's symbol tables.
// A runtime named on a link line without a declaration (`-l:libstdc++.so.6`)
// is the same island, undeclared: verified alike, and reported so the package
// that wrote it declares it.
module;
#include <cstdio>

export module mcpp.build.cxx_runtime;

import std;
import mcpp.runtime.elf;

export namespace mcpp::build::cxx_runtime {

enum class Runtime { None, Libstdcxx, Libcxx };

std::string_view name_of(Runtime r);
Runtime runtime_of_stdlib(std::string_view stdlibId);
// The runtime a link word names: `-lstdc++`, `-l:libstdc++.so.6`, a path to
// `libstdc++.so.6`, `-lsupc++`; `-lc++`, `-lc++abi`, `libc++.so.1`, `libc++.a`.
Runtime runtime_named_by(std::string_view word);
// The runtime a DT_NEEDED soname is.
Runtime runtime_of_soname(std::string_view soname);

// The two classes of the collision set (§6.3): names both runtimes define.
// The ABI layer is bound to ONE runtime as a whole; a layout-dependent name
// differs in object layout between the two and must not be referenced across.
bool abi_layer_symbol(std::string_view mangled);
bool layout_dependent_symbol(std::string_view mangled);

// Is this version string one of a runtime's own symbol versions?
bool runtime_version(Runtime r, std::string_view version);

// Conditions 4–6 for one linked image. `islandObjects` are the objects
// compiled against the other runtime; `searchDirs` locate the image's
// DT_NEEDED libraries. nullopt when every condition holds.
struct ImageCheck {
    std::filesystem::path              image;
    Runtime                            own = Runtime::None;
    Runtime                            foreign = Runtime::None;
    std::vector<std::filesystem::path> islandObjects;
    std::vector<std::filesystem::path> searchDirs;
};
std::optional<std::string> verify_image(const ImageCheck& in);

}  // namespace mcpp::build::cxx_runtime

namespace mcpp::build::cxx_runtime {

std::string_view name_of(Runtime r) {
    switch (r) {
        case Runtime::Libstdcxx: return "libstdc++";
        case Runtime::Libcxx:    return "libc++";
        case Runtime::None:      break;
    }
    return "";
}

Runtime runtime_of_stdlib(std::string_view id) {
    if (id == "libstdc++") return Runtime::Libstdcxx;
    if (id == "libc++") return Runtime::Libcxx;
    return Runtime::None;
}

namespace {
// A library's base name -- `stdc++` for `libstdc++.so.6`, `c++abi` for
// `libc++abi.a` -- or empty when `file` is not a library file name.
std::string_view library_stem(std::string_view file) {
    if (!file.starts_with("lib")) return {};
    auto rest = file.substr(3);
    for (std::string_view ext : {".so", ".a"}) {
        if (auto at = rest.find(ext); at != std::string_view::npos) {
            const auto tail = rest.substr(at + ext.size());
            if (tail.empty() || tail.front() == '.') return rest.substr(0, at);
        }
    }
    return {};
}
Runtime runtime_of_stem(std::string_view stem) {
    if (stem == "stdc++" || stem == "supc++") return Runtime::Libstdcxx;
    if (stem == "c++" || stem == "c++abi") return Runtime::Libcxx;
    return Runtime::None;
}
}  // namespace

Runtime runtime_named_by(std::string_view word) {
    if (word.starts_with("-l:")) return runtime_of_stem(library_stem(word.substr(3)));
    if (word.starts_with("-l")) return runtime_of_stem(word.substr(2));
    if (word.starts_with("-")) return Runtime::None;
    const auto slash = word.find_last_of("/\\");
    return runtime_of_stem(library_stem(slash == std::string_view::npos ? word : word.substr(slash + 1)));
}

Runtime runtime_of_soname(std::string_view soname) {
    return runtime_of_stem(library_stem(soname));
}

bool abi_layer_symbol(std::string_view s) {
    return s.starts_with("__cxa_") || s.starts_with("__gxx_personality")
        || s == "__dynamic_cast" || s.starts_with("_Znw") || s.starts_with("_Zna")
        || s.starts_with("_Zdl") || s.starts_with("_Zda")
        || s.starts_with("_ZTVN10__cxxabiv") || s.starts_with("_ZTIN10__cxxabiv")
        || s.starts_with("_ZNSt9type_info") || s.starts_with("_ZNSt9exception")
        || s.starts_with("_ZTISt9exception") || s.starts_with("_ZTVSt9exception")
        || s.starts_with("_ZNSt9bad_alloc") || s.starts_with("_ZTISt9bad_alloc")
        || s.starts_with("_ZTVSt9bad_alloc") || s.starts_with("_ZSt9terminate")
        || s.starts_with("_ZSt13set_terminate") || s.starts_with("_ZSt14get_terminate");
}

bool layout_dependent_symbol(std::string_view s) {
    // The classes of the `std::exception` family that hold a string, and so
    // differ in layout between the two runtimes. Only names outside libc++'s
    // `__1` namespace collide at all (`St` is `std::` itself).
    static constexpr std::string_view kClasses[] = {
        "St11logic_error", "St12domain_error", "St16invalid_argument",
        "St12length_error", "St12out_of_range", "St13runtime_error",
        "St11range_error", "St14overflow_error", "St15underflow_error",
    };
    for (auto c : kClasses)
        if (s.find(c) != std::string_view::npos) return true;
    return false;
}

bool runtime_version(Runtime r, std::string_view v) {
    if (r == Runtime::Libstdcxx) return v.starts_with("GLIBCXX_") || v.starts_with("CXXABI_");
    return false;   // libc++ and libc++abi publish unversioned symbols
}

namespace {
std::optional<std::filesystem::path> locate(std::string_view soname,
                                            const std::vector<std::filesystem::path>& dirs) {
    std::error_code ec;
    for (auto const& d : dirs) {
        auto p = d / std::string(soname);
        if (std::filesystem::exists(p, ec)) return p;
    }
    return std::nullopt;
}

std::string listed(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < 5; ++i)
        out += std::format("\n         {}", names[i]);
    if (names.size() > 5) out += std::format("\n         … and {} more", names.size() - 5);
    return out;
}
}  // namespace

std::optional<std::string> verify_image(const ImageCheck& in) {
    namespace elf = mcpp::platform::elf;
    const auto foreign = name_of(in.foreign);
    const auto own = name_of(in.own);

    // Condition 4: island objects and the layout-dependent names.
    std::vector<std::string> layout;
    for (auto const& o : in.islandObjects) {
        auto undef = elf::undefined_object_symbols(o);
        if (!undef) continue;
        for (auto const& n : *undef)
            if (layout_dependent_symbol(n)) layout.push_back(std::format("{}  ({})", n, o.filename().string()));
    }
    if (!layout.empty())
        return std::format(
            "{}: an object compiled against {} references a class whose layout differs in {} "
            "[cxx-runtime-collision]:{}\n"
            "       Its C interface must not pass these across. Build the package against {} "
            "(remove its private requirement), or build with a {} toolchain.",
            in.image.filename().string(), foreign, own, listed(layout), own, foreign);

    auto imports = elf::versioned_dynamic_imports(in.image);
    if (!imports) return std::nullopt;   // not ELF, or unreadable: nothing to verify here

    // Condition 5: the ABI layer stays whole, on the image's own runtime.
    std::vector<std::string> split;
    for (auto const& i : *imports)
        if (abi_layer_symbol(i.name) && runtime_version(in.foreign, i.version))
            split.push_back(std::format("{}@{}", i.name, i.version));
    if (!split.empty())
        return std::format(
            "{}: the C++ ABI layer is split between {} and {} [cxx-runtime-collision]: these "
            "are imported from {}:{}\n"
            "       A throw from one runtime is then caught by the other, and a typed `catch` "
            "misses (mcpp#838). The build's own runtime must answer them; this link names "
            "{} before it.",
            in.image.filename().string(), own, foreign, foreign, listed(split), foreign);

    // Condition 6: no typeinfo crosses. A typeinfo imported at the other
    // runtime's versions is one of its own classes; one from a library that
    // is built on the other runtime belongs to that library's exceptions,
    // thrown by the other runtime's `__cxa_throw` -- foreign to this image's
    // personality, so only `catch (...)` would catch them.
    //
    // The image's typeinfo is what it imports AND what it defines: a class
    // whose type is caught usually has no key function, so the catch clause
    // carries its own vague-linkage copy of the typeinfo and imports nothing
    // -- matched at run time by name against the thrower's, which the other
    // runtime's personality never gets to compare.
    std::vector<std::string> crossing;
    std::set<std::string> ourTypeinfo;
    for (auto const& i : *imports) {
        if (!i.name.starts_with("_ZTI")) continue;
        if (runtime_version(in.foreign, i.version))
            crossing.push_back(std::format("{}@{}", i.name, i.version));
        else
            ourTypeinfo.insert(i.name);
    }
    if (auto defined = elf::defined_object_symbols(in.image))
        for (auto const& n : *defined)
            if (n.starts_with("_ZTI")) ourTypeinfo.insert(n);
    if (auto facts = elf::inspect_elf_runtime(in.image)) {
        auto dirs = in.searchDirs;
        for (auto const& r : facts->runpaths) dirs.emplace_back(r);
        for (auto const& needed : facts->needed) {
            if (runtime_of_soname(needed) != Runtime::None) continue;
            auto path = locate(needed, dirs);
            if (!path) continue;
            auto lib = elf::inspect_elf_runtime(*path);
            if (!lib || std::ranges::none_of(lib->needed, [&](const std::string& n) {
                    return runtime_of_soname(n) == in.foreign; }))
                continue;
            auto defined = elf::inspect_dynamic_symbols(*path);
            if (!defined) continue;
            std::set<std::string> theirs;
            for (auto const& d : defined->defined)
                if (d.name.starts_with("_ZTI")) theirs.insert(d.name);
            for (auto const& t : ourTypeinfo)
                if (!abi_layer_symbol(t) && theirs.contains(t))
                    crossing.push_back(std::format("{}  (also in {}, built on {})", t, needed, foreign));
        }
    }
    if (!crossing.empty())
        return std::format(
            "{}: a type is caught or cast across C++ runtimes [cxx-runtime-collision]:{}\n"
            "       Exceptions of these types are thrown by {}, and this image's {} cannot match "
            "them in a typed `catch`. Build with one runtime: depend on a package that provides "
            "`mcpp:c++-abi={}`, or build with a toolchain whose runtime it is.",
            in.image.filename().string(), listed(crossing), foreign, own, foreign);
    return std::nullopt;
}

}  // namespace mcpp::build::cxx_runtime
