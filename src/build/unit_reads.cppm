// mcpp.build.unit_reads -- the files a compile read, as the build directory that ran it records
// them: the object's inputs in ninja's deps log (a GNU depfile's first record, or the paths
// cl.exe's `/sourceDependencies` named, which the executor hands ninja as `deps = msvc` lines),
// and the scan's depfile (`<object dir>/<source name>.ddi.dep`). One reader for every consumer:
// the tests a change affects (mcpp.build.test_impact) and the inputs of a build program
// (design 2026-11-21, build programs' inputs §3.1).
//
// It takes paths, not the plan: with `import mcpp.build.plan` here, GCC 16.1 crashed reading the
// BMI of mcpp.cli, which reaches this module through mcpp.build.test_impact (a segmentation
// fault in read_namespaces, merging an anonymous namespace of mcpp.toolchain.target).
export module mcpp.build.unit_reads;

import std;
import mcpp.build.unit_cache;   // depfile_prerequisites
import mcpplibs.os;

export namespace mcpp::build::unit_reads {

// The inputs of each output, by the output's path as ninja names it.
struct DepsLog {
    std::map<std::string, std::vector<std::string>> inputs;
};

// THE FILES EACH OUTPUT READ, as ninja's deps log states them (`.ninja_deps`, format 4: a magic
// line, a version, then records whose size word's high bit tells a dependency record from a
// path record). A later record of an output replaces an earlier one, as in ninja. Nothing when
// the log is absent or of another version.
DepsLog ninja_deps(const std::filesystem::path& file);

// The files the unit compiled from `source` to `object` (relative to the build directory
// `outputDir`) read when that directory last built it, absolute and normal, each once; nullopt
// when the directory holds no record of it.
std::optional<std::vector<std::filesystem::path>>
reads_of(const std::filesystem::path& outputDir, const std::filesystem::path& object,
         const std::filesystem::path& source, const DepsLog& deps);

}  // namespace mcpp::build::unit_reads

namespace mcpp::build::unit_reads {

namespace detail {

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace detail

DepsLog ninja_deps(const std::filesystem::path& file) {
    DepsLog out;
    const auto bytes = detail::read_text(file);
    constexpr std::string_view kMagic = "# ninjadeps\n";
    if (!bytes.starts_with(kMagic) || bytes.size() < kMagic.size() + 4) return out;
    auto word = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    };
    if (word(kMagic.size()) != 4) return out;
    std::vector<std::string> paths;
    for (std::size_t at = kMagic.size() + 4; at + 4 <= bytes.size();) {
        const auto head = word(at);
        const std::size_t size = head & 0x7FFFFFFFu;
        at += 4;
        if (at + size > bytes.size() || size < 4) break;
        if ((head & 0x80000000u) == 0) {
            std::string path = bytes.substr(at, size - 4);
            while (!path.empty() && path.back() == '\0') path.pop_back();
            paths.push_back(std::move(path));
        } else if (const auto id = word(at); id < paths.size() && size >= 12) {
            auto& inputs = out.inputs[paths[id]];
            inputs.clear();
            for (std::size_t i = at + 12; i + 4 <= at + size; i += 4)
                if (const auto in = word(i); in < paths.size()) inputs.push_back(paths[in]);
        }
        at += size;
    }
    return out;
}

std::optional<std::vector<std::filesystem::path>>
reads_of(const std::filesystem::path& outputDir, const std::filesystem::path& object,
         const std::filesystem::path& source, const DepsLog& deps) {
    bool recorded = false;
    std::vector<std::filesystem::path> out;
    auto add = [&](const std::string& in) {
        const auto file = mcpplibs::os::fs::from_utf8(in);
        auto abs = (file.is_absolute() ? file : outputDir / file).lexically_normal();
        if (std::ranges::find(out, abs) == out.end()) out.push_back(std::move(abs));
    };
    if (auto it = deps.inputs.find(object.generic_string()); it != deps.inputs.end()) {
        recorded = true;
        std::ranges::for_each(it->second, add);
    }
    const auto scan = outputDir / object.parent_path() / (source.filename().string() + ".ddi.dep");
    if (const auto text = detail::read_text(scan); !text.empty()) {
        recorded = true;
        std::ranges::for_each(mcpp::build::unit_cache::depfile_prerequisites(text), add);
    }
    if (!recorded) return std::nullopt;
    return out;
}

}  // namespace mcpp::build::unit_reads
