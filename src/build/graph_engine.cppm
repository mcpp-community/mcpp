// mcpp.build.graph_engine -- the engine a build directory's graph runs
// (design 2026-11-12 §17, W1).
//
// The graph runs this engine in its commands -- the capped compile
// (`$mcpp __compiler-cap ... -- <compiler>`), BMI comparison, link wrapping,
// staging, module bindings -- and ninja compares each command with the one it
// last ran. This engine's own path changes with each release
// (`xim-x-mcpp/<version>/bin/mcpp`), so a graph that named it there changed
// every command on an upgrade, and every unit was compiled again although the
// compiler would have written the same objects (measured: every unit of a
// workspace's members). The graph names the build directory's own link to the
// engine instead, which the planner makes current under the directory's lock:
// a command then changes with its compiler, its flags and its files. The link's
// path carries the epoch of what the engine writes (kCacheEpoch, which the
// planner passes): a release that changes what the engine's steps write bumps
// it, and every edge runs again.
export module mcpp.build.graph_engine;

import std;
import mcpplibs.os;
import mcpp.invocation;

export namespace mcpp::build::graph_engine {

// The link a build directory's graph names: `<dir>/engine/epoch-<N>/mcpp`.
std::filesystem::path link(const std::filesystem::path& outputDir, int epoch);

// Makes the link this engine -- a hard link where the file system makes one,
// else a copy -- and returns what the graph names: the link, or this engine's
// own path when neither could be made. Called under the directory's lock.
std::filesystem::path refresh(const std::filesystem::path& outputDir, int epoch);

// The link at `to` (the path a plan's graph names), made again where it is
// missing: a cold build (`--cache off`) empties the directory after the plan
// named it. Returns `to`, or the running engine's own path when no link can be
// made there.
std::filesystem::path place(const std::filesystem::path& to);

// Whether `named`, the engine a graph names, is this engine: its own path, or
// a link that is the same file or a copy of it.
bool is_this_engine(const std::filesystem::path& named);

} // namespace mcpp::build::graph_engine

namespace mcpp::build::graph_engine {

namespace {
// The same file, or a copy of its size and time.
bool same_engine(const std::filesystem::path& a, const std::filesystem::path& b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::equivalent(a, b, ec) && !ec) return true;
    std::error_code s1, s2, t1, t2;
    return fs::file_size(a, s1) == fs::file_size(b, s2) && !s1 && !s2
        && fs::last_write_time(a, t1) == fs::last_write_time(b, t2) && !t1 && !t2;
}
} // namespace

std::filesystem::path link(const std::filesystem::path& outputDir, int epoch) {
    return outputDir / "engine" / std::format("epoch-{}", epoch)
         / (std::string("mcpp") + std::string(mcpplibs::os::exe_suffix));
}

bool is_this_engine(const std::filesystem::path& named) {
    const auto self = mcpp::invocation::self_exe_path();
    return named == self || same_engine(named, self);
}

namespace {
// What the link is made from: the running engine's file, or, where that file
// was replaced while it ran (Linux names it `<path> (deleted)`, as when an
// engine rebuilds itself in its own repository), the running image itself.
std::filesystem::path source_of(const std::filesystem::path& self) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(self, ec)) return self;
    // A system that presents the running image as a file (procfs) reads it
    // there whether or not its path still names it.
    const std::filesystem::path image{"/proc/self/exe"};
    if (std::filesystem::is_regular_file(image, ec)) return image;
    return self;
}
} // namespace

std::filesystem::path place(const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    const auto self = mcpp::invocation::self_exe_path();
    if (same_engine(to, self)) return to;
    const auto from = source_of(self);
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    if (from == self) fs::create_hard_link(from, to, ec);
    if (from != self || ec) {
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (!ec && from == self) fs::last_write_time(to, fs::last_write_time(self, ec), ec);
    }
    if (!ec) return to;
    std::error_code rec;
    fs::remove(to, rec);
    return self;
}

std::filesystem::path refresh(const std::filesystem::path& outputDir, int epoch) {
    namespace fs = std::filesystem;
    const auto to = link(outputDir, epoch);
    if (same_engine(to, mcpp::invocation::self_exe_path())) return to;
    std::error_code ec;
    // A link of another epoch is named by no graph this engine writes.
    fs::remove_all(to.parent_path().parent_path(), ec);
    return place(to);
}

} // namespace mcpp::build::graph_engine
