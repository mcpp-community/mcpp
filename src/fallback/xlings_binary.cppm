// mcpp.fallback.xlings_binary — xlings binary acquisition chain.
//
// Tries multiple strategies to obtain the xlings binary:
//   1. MCPP_VENDORED_XLINGS env var (explicit override)
//   2. system `which xlings`
//   3. Fail with user-facing instructions

module;
#include <cstdlib>

export module mcpp.fallback.xlings_binary;

import std;
import mcpp.platform;

export namespace mcpp::fallback {

// Try to acquire (copy) the xlings binary to destBin.
// Returns destBin on success or an error string.
std::expected<std::filesystem::path, std::string>
acquire_xlings_binary(const std::filesystem::path& destBin, bool quiet = false) {
    if (std::filesystem::exists(destBin)) return destBin;

    std::error_code ec;
    std::filesystem::create_directories(destBin.parent_path(), ec);

    // Right-pad verb to 12 columns (matches mcpp::ui::verb_padded layout).
    auto print_status = [](std::string_view verb, std::string_view msg) {
        constexpr std::size_t W = 12;
        if (verb.size() >= W)
            std::println("{} {}", verb, msg);
        else
            std::println("{}{} {}", std::string(W - verb.size(), ' '), verb, msg);
    };

    // 1. Explicit override
    if (auto* e = std::getenv("MCPP_VENDORED_XLINGS"); e && *e) {
        std::filesystem::path src{e};
        if (std::filesystem::exists(src)) {
            std::filesystem::copy_file(src, destBin,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::permissions(destBin,
                    std::filesystem::perms::owner_exec
                  | std::filesystem::perms::group_exec
                  | std::filesystem::perms::others_exec,
                  std::filesystem::perm_options::add, ec);
                if (!quiet) print_status("Bundled",
                    std::format("xlings (from MCPP_VENDORED_XLINGS)"));
                return destBin;
            }
        }
    }

    // 2. Copy from system (`which xlings`)
    auto xlings_name = std::string("xlings") + std::string(mcpp::platform::exe_suffix);
    auto sysXlings = mcpp::platform::fs::which(xlings_name);
    if (sysXlings) {
        std::string p = sysXlings->string();
        if (!p.empty() && std::filesystem::exists(p)) {
            std::filesystem::copy_file(p, destBin,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::permissions(destBin,
                    std::filesystem::perms::owner_exec
                  | std::filesystem::perms::group_exec
                  | std::filesystem::perms::others_exec,
                  std::filesystem::perm_options::add, ec);
                if (!quiet) print_status("Bundled",
                    std::format("xlings (copied from system: {})", p));
                return destBin;
            }
        }
    }

    // 3. Fail with instructions
    return std::unexpected(std::format(
        "xlings binary not found. Either:\n"
        "  - install via: curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/refs/heads/main/tools/other/quick_install.sh | bash\n"
        "  - export MCPP_VENDORED_XLINGS=/abs/path/to/xlings\n"
        "  - set [xlings].binary = \"system\" in {}",
        (destBin.parent_path().parent_path() / "config.toml").string()));
}

} // namespace mcpp::fallback
