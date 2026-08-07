// mcpp.ide.publish — atomically persist configured IDE snapshot metadata.
export module mcpp.ide.publish;

import std;
import mcpp.build.compile_commands;
import mcpp.ide.model;
import mcpp.libs.json;
import mcpp.platform.fs;

export namespace mcpp::ide {

struct PublishedSnapshot {
    std::string projectId;
    std::string configurationId;
    std::string snapshotId;
    std::string phase = "configured";
    std::filesystem::path projectRoot;
    std::filesystem::path compileCommands;
    std::filesystem::path compatibilityCompileCommands;
    std::string toolchain;
    std::string toolchainFingerprint;
    std::size_t compileCommandCount = 0;
};

namespace detail {

using Json = nlohmann::ordered_json;

Json snapshot_json(const PublishedSnapshot& snapshot) {
    auto json = Json::object();
    json["schemaVersion"] = 1;
    json["kind"] = "mcpp.ide.configured-snapshot";
    json["phase"] = snapshot.phase;
    json["projectId"] = snapshot.projectId;
    json["configurationId"] = snapshot.configurationId;
    json["snapshotId"] = snapshot.snapshotId;
    json["projectRoot"] = snapshot.projectRoot.generic_string();
    json["compileCommands"] = snapshot.compileCommands.generic_string();
    json["compatibilityCompileCommands"] =
        snapshot.compatibilityCompileCommands.generic_string();
    json["compileCommandCount"] = snapshot.compileCommandCount;
    json["toolchain"] = snapshot.toolchain;
    json["toolchainFingerprint"] = snapshot.toolchainFingerprint;
    return json;
}

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::expected<void, std::string>
write_bytes_atomically(const std::filesystem::path& path,
                       std::string_view content,
                       std::string_view description) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected(std::format(
        "cannot create {} directory '{}': {}", description,
        path.parent_path().string(), ec.message()));

    auto temp = path;
    temp += std::format(".tmp-{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return std::unexpected(std::format(
            "cannot write {} '{}'", description, temp.string()));
        output << content;
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temp, ec);
            return std::unexpected(std::format(
                "cannot flush {} '{}'", description, temp.string()));
        }
    }

    mcpp::platform::fs::replace_file(temp, path, ec);
    if (ec) {
        const auto publishError = ec;
        std::filesystem::remove(temp, ec);
        return std::unexpected(std::format(
            "cannot publish {} '{}': {}", description,
            path.string(), publishError.message()));
    }
    return {};
}

std::expected<void, std::string>
write_json_atomically(const std::filesystem::path& path, std::string_view content) {
    auto parsed = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object())
        return std::unexpected("IDE snapshot metadata must be a JSON object");
    return write_bytes_atomically(path, content, "IDE snapshot metadata");
}

std::expected<std::optional<std::string>, std::string>
read_optional_file(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return std::unexpected(std::format(
        "cannot inspect compatibility CDB '{}': {}", path.string(), ec.message()));
    if (!exists) return std::optional<std::string>{};
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::unexpected(std::format(
            "compatibility CDB is not a regular file: '{}'", path.string()));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(std::format(
        "cannot read compatibility CDB '{}'", path.string()));
    std::string content(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) return std::unexpected(std::format(
        "cannot finish reading compatibility CDB '{}'", path.string()));
    return std::optional<std::string>(std::move(content));
}

std::expected<void, std::string>
restore_optional_file(const std::filesystem::path& path,
                      const std::optional<std::string>& previous) {
    if (previous) return write_bytes_atomically(
        path, *previous, "previous compatibility CDB");

    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) return std::unexpected(std::format(
        "cannot remove newly published compatibility CDB '{}': {}",
        path.string(), ec.message()));
    return {};
}

std::expected<PublishedSnapshot, std::string> parse_snapshot(const Json& json) {
    const auto string_field = [&](std::string_view key)
        -> std::expected<std::string, std::string> {
        if (!json.contains(key) || !json[key].is_string()
            || json[key].get_ref<const std::string&>().empty()) {
            return std::unexpected(std::format(
                "IDE snapshot metadata has invalid '{}'", key));
        }
        return json[key].get<std::string>();
    };

    if (!json.is_object()
        || !json.contains("schemaVersion") || !json["schemaVersion"].is_number_integer()
        || json["schemaVersion"].get<int>() != 1
        || !json.contains("kind") || !json["kind"].is_string()
        || json["kind"].get_ref<const std::string&>()
            != "mcpp.ide.configured-snapshot") {
        return std::unexpected("unsupported IDE snapshot metadata");
    }
    auto projectId = string_field("projectId");
    auto configurationId = string_field("configurationId");
    auto snapshotId = string_field("snapshotId");
    auto phase = string_field("phase");
    auto projectRoot = string_field("projectRoot");
    auto compileCommands = string_field("compileCommands");
    auto compatibility = string_field("compatibilityCompileCommands");
    auto toolchain = string_field("toolchain");
    auto toolchainFingerprint = string_field("toolchainFingerprint");
    if (!projectId) return std::unexpected(projectId.error());
    if (!configurationId) return std::unexpected(configurationId.error());
    if (!snapshotId) return std::unexpected(snapshotId.error());
    if (!phase) return std::unexpected(phase.error());
    if (!projectRoot) return std::unexpected(projectRoot.error());
    if (!compileCommands) return std::unexpected(compileCommands.error());
    if (!compatibility) return std::unexpected(compatibility.error());
    if (!toolchain) return std::unexpected(toolchain.error());
    if (!toolchainFingerprint) return std::unexpected(toolchainFingerprint.error());

    std::size_t compileCommandCount = 0;
    if (json.contains("compileCommandCount")) {
        if (!json["compileCommandCount"].is_number_unsigned())
            return std::unexpected(
                "IDE snapshot metadata has invalid 'compileCommandCount'");
        compileCommandCount = json["compileCommandCount"].get<std::size_t>();
    }

    return PublishedSnapshot{
        .projectId = std::move(*projectId),
        .configurationId = std::move(*configurationId),
        .snapshotId = std::move(*snapshotId),
        .phase = std::move(*phase),
        .projectRoot = std::filesystem::path(std::move(*projectRoot)),
        .compileCommands = std::filesystem::path(std::move(*compileCommands)),
        .compatibilityCompileCommands = std::filesystem::path(std::move(*compatibility)),
        .toolchain = std::move(*toolchain),
        .toolchainFingerprint = std::move(*toolchainFingerprint),
        .compileCommandCount = compileCommandCount,
    };
}

std::expected<void, std::string>
validate_snapshot_paths(const std::filesystem::path& projectRoot,
                        const PublishedSnapshot& snapshot) {
    const auto physicalRoot = physical_project_root(projectRoot);
    if (snapshot.projectRoot.empty() || !snapshot.projectRoot.is_absolute()
        || physical_project_root(snapshot.projectRoot) != physicalRoot) {
        return std::unexpected(
            "configured snapshot project root does not match publication root");
    }
    if (snapshot.compileCommands.empty() || !snapshot.compileCommands.is_absolute()) {
        return std::unexpected(
            "configured snapshot CDB path must be absolute");
    }
    if (snapshot.compatibilityCompileCommands.empty()
        || !snapshot.compatibilityCompileCommands.is_absolute()
        || physical_project_root(snapshot.compatibilityCompileCommands)
            != physicalRoot / "compile_commands.json") {
        return std::unexpected(
            "configured snapshot compatibility CDB does not belong to project root");
    }

    // current.json 不是可信输入；读取时重做与发布时相同的物理路径约束，
    // 防止损坏或被修改的 metadata 把 IDE 引向工程外的 CDB。
    const auto repliesRoot = physicalRoot / ".mcpp" / "ide" / "replies";
    const auto physicalCdb = physical_project_root(snapshot.compileCommands);
    const auto relativeCdb = physicalCdb.lexically_relative(repliesRoot);
    if (relativeCdb.empty() || relativeCdb.is_absolute()
        || *relativeCdb.begin() == "..") {
        return std::unexpected(std::format(
            "configured snapshot CDB escapes IDE replies directory: '{}'",
            snapshot.compileCommands.string()));
    }
    return {};
}

} // namespace detail

std::expected<void, std::string>
publish_current_snapshot(const std::filesystem::path& projectRoot,
                         const PublishedSnapshot& snapshot) {
    const auto physicalRoot = physical_project_root(projectRoot);
    if (auto valid = detail::validate_snapshot_paths(physicalRoot, snapshot); !valid)
        return valid;

    const auto physicalCdb = physical_project_root(snapshot.compileCommands);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(physicalCdb, ec) || ec) {
        return std::unexpected(std::format(
            "configured snapshot CDB is unavailable: '{}'",
            snapshot.compileCommands.string()));
    }

    const auto content = detail::snapshot_json(snapshot).dump(2) + "\n";
    const auto ideRoot = physicalRoot / ".mcpp" / "ide";
    const auto reply = ideRoot / "replies" / std::format(
        "snapshot-{:016x}.json", detail::fnv1a64(content));
    if (auto result = detail::write_json_atomically(reply, content); !result)
        return result;

    // current.json 是最后一个切换点；此前任何失败都只留下不可见的 reply。
    return detail::write_json_atomically(ideRoot / "current.json", content);
}

std::expected<void, std::string>
publish_configured_snapshot(const std::filesystem::path& projectRoot,
                            const PublishedSnapshot& snapshot,
                            std::string_view compileCommandsContent) {
    const auto physicalRoot = physical_project_root(projectRoot);
    if (auto valid = detail::validate_snapshot_paths(physicalRoot, snapshot); !valid)
        return valid;

    // 同一工程的两个 configure 可以并行完成解析，但 mutable 指针只能串行
    // 切换；否则旧内容备份和失败回滚可能覆盖另一个进程刚发布的配置。
    const auto publicationDir = physicalRoot / ".mcpp" / "ide";
    std::error_code lockEc;
    auto publicationLock = mcpp::platform::fs::FileLock::try_acquire(
        publicationDir, lockEc);
    if (!publicationLock) {
        if (lockEc) {
            return std::unexpected(std::format(
                "cannot acquire IDE publication lock '{}': {}",
                (publicationDir / ".lock").string(), lockEc.message()));
        }
        return std::unexpected(
            "another IDE snapshot publication is already in progress");
    }

    auto previous = detail::read_optional_file(snapshot.compatibilityCompileCommands);
    if (!previous) return std::unexpected(previous.error());

    if (auto compatibility = mcpp::build::write_fresh_compile_commands(
            snapshot.compatibilityCompileCommands, compileCommandsContent);
        !compatibility) {
        return compatibility;
    }

    if (auto current = publish_current_snapshot(projectRoot, snapshot); !current) {
        // current.json 是客户端的最终切换点。若它发布失败，根 CDB 也必须
        // 回到调用前状态，避免命令失败却让 clangd 悄悄切到新配置。
        auto restored = detail::restore_optional_file(
            snapshot.compatibilityCompileCommands, *previous);
        if (!restored) return std::unexpected(std::format(
            "{}; additionally failed to restore compatibility CDB: {}",
            current.error(), restored.error()));
        return current;
    }
    return {};
}

std::expected<std::optional<PublishedSnapshot>, std::string>
read_current_snapshot(const std::filesystem::path& projectRoot) {
    const auto path = projectRoot / ".mcpp" / "ide" / "current.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) return std::unexpected(std::format(
            "cannot inspect IDE snapshot metadata '{}': {}", path.string(), ec.message()));
        return std::optional<PublishedSnapshot>{};
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::unexpected(std::format(
            "IDE snapshot metadata is not a regular file: '{}'", path.string()));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(std::format(
        "cannot read IDE snapshot metadata '{}'", path.string()));
    auto json = detail::Json::parse(input, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) return std::unexpected(std::format(
        "IDE snapshot metadata is invalid JSON: '{}'", path.string()));
    auto parsed = detail::parse_snapshot(json);
    if (!parsed) return std::unexpected(parsed.error());
    if (auto valid = detail::validate_snapshot_paths(projectRoot, *parsed); !valid)
        return std::unexpected(valid.error());
    return std::optional<PublishedSnapshot>(std::move(*parsed));
}

} // namespace mcpp::ide
