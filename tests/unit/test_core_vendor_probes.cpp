#include <gtest/gtest.h>

import std;

// The engine owns no vendor probes.
//
// The bound a device toolkit states for its host compiler, whether a device
// compiler can reach its own back-end, and which driver a machine has are
// facts about one vendor's tools. They were once read in `src/doctor.cppm` and
// `src/toolchain/devicehost.cppm`, and every one of them was spelled CUDA.
// Moving them into the rule package that drives the tools is what keeps the
// engine from growing four copies -- AMD, Intel and Apple each have their own
// bound and their own back-end reachability question.
//
// What the engine keeps is the shape: the accel grammar (backend, version,
// architecture set, floor), the probe channel (`mcpp:fact` / `mcpp:floor`)
// and the comparison. None of those names a tool.
//
// The words below are assembled from pieces so this file does not flag itself
// should it ever move under src/.

namespace {

std::string without_comments(std::string_view source) {
    enum class State { Code, Line, Block, String, Character } state = State::Code;
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char n = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (c == '/' && n == '/') { state = State::Line; ++i; out += "  "; }
            else if (c == '/' && n == '*') { state = State::Block; ++i; out += "  "; }
            else {
                out.push_back(c);
                if (c == '"') state = State::String;
                else if (c == '\'') state = State::Character;
            }
        } else if (state == State::Line) {
            if (c == '\n') { state = State::Code; out.push_back(c); }
            else out.push_back(' ');
        } else if (state == State::Block) {
            if (c == '*' && n == '/') { state = State::Code; ++i; out += "  "; }
            else out.push_back(c == '\n' ? '\n' : ' ');
        } else {
            out.push_back(c);
            if (c == '\\' && i + 1 < source.size()) out.push_back(source[++i]);
            else if ((state == State::String && c == '"')
                  || (state == State::Character && c == '\'')) state = State::Code;
        }
    }
    return out;
}

}  // namespace

TEST(CoreVendorProbes, TheEngineNamesNoVendorTool) {
    auto repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto src = repo / "src";
    ASSERT_TRUE(std::filesystem::is_directory(src)) << src;
    // Tools, probes and locations. Each one is something only a rule package
    // has business invoking or reading.
    const std::vector<std::string> tools = {
        "nv" "cc", "ci" "cc", "pt" "xas", "fat" "binary", "nvidia" "-smi",
        "cuDriver" "GetVersion", "/usr/local/" "cuda", "host_config" ".h",
        "nvcc" ".profile", "hip" "cc", "rocm" "info", "ic" "px", "sycl" "-ls",
        "cudafe" "++",
    };
    std::size_t files = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(src);
         it != std::filesystem::recursive_directory_iterator{}; ++it) {
        if (!it->is_regular_file()) continue;
        auto ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".cppm") continue;
        ++files;
        std::ifstream input(it->path());
        std::string raw((std::istreambuf_iterator<char>(input)), {});
        auto code = without_comments(raw);
        std::ranges::transform(code, code.begin(),
            [](unsigned char c) { return std::tolower(c); });
        std::size_t line = 0;
        for (auto text : code | std::views::split('\n')) {
            ++line;
            std::string lineText(text.begin(), text.end());
            for (auto const& tool : tools) {
                std::string lowered = tool;
                std::ranges::transform(lowered, lowered.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                EXPECT_EQ(lineText.find(lowered), std::string::npos)
                    << it->path() << ':' << line << " names a vendor tool: " << tool;
            }
        }
    }
    // The denominator: a scan that found no files would pass vacuously.
    EXPECT_GT(files, 100u);
}
