// Platform filesystem primitives used by transactional scaffolding.  The
// public contract is stronger than std::filesystem::rename: an existing final
// name is never replaced, including a target that appears in the commit race.

export module mcpp.platform.scaffold_fs;

import std;
import mcpp.platform.fs_caps;

export namespace mcpp::platform {

std::expected<void, std::string> atomic_rename_directory_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    // By what the target's filesystem does (fs_caps, #809): renameat2's
    // RENAME_NOREPLACE is EINVAL on NFSv3, where `mcpp new` failed.
    if (auto published = rename_no_replace(source, target); !published)
        return std::unexpected(std::format("atomic no-replace rename '{}' -> '{}' failed: {}",
                                           source.string(), target.string(), published.error()));
    return {};
}

// Make a newly written regular file durable before its containing staging
// directory is published. Unsupported filesystem sync operations are treated
// as "best available"; genuine I/O/permission failures abort the transaction.
// Each system's own: posix/ and windows/ scaffold_fs.cpp.
std::expected<void, std::string> sync_regular_file(const std::filesystem::path& path);

// Persist the directory entry after the exclusive rename where the host/filesystem
// offers directory fsync. MoveFileExW(MOVEFILE_WRITE_THROUGH) already supplies
// the corresponding Windows guarantee.
std::expected<void, std::string> sync_directory(const std::filesystem::path& path);

} // namespace mcpp::platform
