// Optional action telemetry. Snapshots describe work inside an action; the
// command's exit status and output postconditions remain authoritative.
export module mcpp.build.action_progress;
import std;
import mcpp.libs.json;
import mcpplibs.os.fs;

export namespace mcpp::build::action_progress {
inline constexpr std::string_view kRootEnv = "MCPP_ACTION_PROGRESS_ROOT";
inline constexpr std::string_view kFileEnv = "MCPP_ACTION_PROGRESS_FILE";
inline constexpr std::size_t kMaxBytes = 8192;
struct Snapshot {
    std::uint64_t seq = 0;
    std::string phase;
    std::optional<std::uint64_t> done, total;
    std::string detail;
};
std::optional<Snapshot> parse(std::string_view text);
std::optional<Snapshot> read(const std::filesystem::path& file);
std::filesystem::path channel(const std::filesystem::path& root, std::string_view output);
std::filesystem::path make_directory(const std::filesystem::path& buildDir);
std::string describe(const Snapshot& snapshot);
}

namespace mcpp::build::action_progress {
namespace {
bool safe_text(const std::string& value, std::size_t limit) {
    return value.size() <= limit && std::ranges::none_of(value, [](unsigned char c) {
        return c < 32 || c == 127;
    });
}
std::optional<std::uint64_t> count(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::uint64_t>();
}
}
std::optional<Snapshot> parse(std::string_view text) {
    if (text.size() > kMaxBytes) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(text);
        if (!j.is_object() || !j.contains("v") || j["v"] != 1) return std::nullopt;
        auto seq = count(j, "seq");
        if (!seq || !j.contains("phase") || !j["phase"].is_string()) return std::nullopt;
        Snapshot out{.seq = *seq, .phase = j["phase"].get<std::string>()};
        if (out.phase.empty() || !safe_text(out.phase, 128)) return std::nullopt;
        out.done = count(j, "done"); out.total = count(j, "total");
        if ((j.contains("done") && !out.done) || (j.contains("total") && !out.total))
            return std::nullopt;
        if (out.done && out.total && *out.done > *out.total) return std::nullopt;
        if (auto detail = j.find("detail"); detail != j.end()) {
            if (!detail->is_string()) return std::nullopt;
            out.detail = detail->get<std::string>();
            if (!safe_text(out.detail, 1024)) return std::nullopt;
        }
        return out;
    } catch (...) { return std::nullopt; }
}
std::optional<Snapshot> read(const std::filesystem::path& file) {
    // Bound the read before JSON parsing, including files which grow while
    // opened. An invalid or transient snapshot cannot fail a build.
    const auto bytes = mcpplibs::os::fs::read(file, kMaxBytes);
    if (!bytes) return std::nullopt;
    return parse(*bytes);
}
std::filesystem::path channel(const std::filesystem::path& root, std::string_view output) {
    // The identity comes from the graph, never from the telemetry document.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : output) { hash ^= c; hash *= 1099511628211ULL; }
    return root / std::format("{:016x}.json", hash);
}
std::filesystem::path make_directory(const std::filesystem::path& buildDir) {
    // Allocated at execution time; neither this nonce nor snapshots are graph
    // inputs. create_directory is the collision check, even across processes.
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        auto candidate = buildDir / std::format(".mcpp-action-progress-{:x}-{:x}",
            std::chrono::steady_clock::now().time_since_epoch().count(), attempt);
        if (std::filesystem::create_directory(candidate, ec))
            return std::filesystem::absolute(candidate, ec);
        ec.clear();
    }
    return {};
}
std::string describe(const Snapshot& snapshot) {
    std::string text = snapshot.phase;
    if (snapshot.done) {
        text += std::format(" {}", *snapshot.done);
        if (snapshot.total) text += std::format("/{}", *snapshot.total);
    }
    if (!snapshot.detail.empty()) text += " · " + snapshot.detail;
    return text;
}
}
