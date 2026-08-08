// mcpp.fallback.xlings_binary — xlings binary acquisition chain.
//
// Tries multiple strategies to obtain the xlings binary:
//   1. MCPP_VENDORED_XLINGS env var (explicit override)
//   2. system `which xlings`
//   3. Fail with user-facing instructions

module;
#include <cstdio>
#include <cstdlib>
#include <cctype>

export module mcpp.fallback.xlings_binary;

import std;
import mcpp.platform;

export namespace mcpp::fallback {

// Try to acquire (copy) the xlings binary to destBin.
// Returns destBin on success or an error string.
// The version already vendored at destBin, or empty when it cannot be read.
std::string vendored_xlings_version(const std::filesystem::path& bin);

// The version the acquisition chain WOULD install, without installing it.
// Empty when nothing is available. Replacing a vendored binary is only an
// improvement when this is newer than what is already there.
std::string candidate_source_version();

// True when `have` is strictly older than `want`, comparing dot-separated
// numeric components. Anything unparseable answers false -- a version this
// code does not understand is not evidence of being behind.
bool version_is_older(std::string_view have, std::string_view want);

// Try to acquire (copy) the xlings binary to destBin.
//
// `pinnedVersion` is the version this mcpp expects. A vendored binary OLDER
// than the pin is replaced; one that is newer or equal is left alone.
//
// This used to return early on mere existence, with no version comparison at
// all, and nothing else ever revisited the file. A home created once kept
// whatever xlings it first acquired forever -- measured at 2026.8.2.1 against
// a pin of 2026.8.6.3, with `mcpp self env` printing both numbers side by side
// and saying nothing about the gap. The cost is not cosmetic: features mcpp
// reads from xlings simply do not appear. The subos_info block arrived in
// 2026.8.5.1, so on that machine the graphics packages' declarations were
// discarded by a client too old to have the API, and mcpp#352's fix could
// never take effect.
//
// Strictly-older, not not-equal: a user who put a newer xlings there on
// purpose must not be downgraded by an mcpp that happens to pin an older one.
std::expected<std::filesystem::path, std::string>
acquire_xlings_binary(const std::filesystem::path& destBin, bool quiet = false,
                      std::string_view pinnedVersion = {}) {
    if (std::filesystem::exists(destBin)) {
        auto have = vendored_xlings_version(destBin);
        if (pinnedVersion.empty() || have.empty()
            || !version_is_older(have, pinnedVersion))
            return destBin;

        // Behind the pin -- but replacing is only an improvement if what we
        // would put there is actually newer. The acquisition chain below ends
        // at whatever `which xlings` finds, and on a machine whose system
        // xlings is ANCIENT that is a downgrade dressed up as an update.
        //
        // Learned by doing it: this code first deleted the vendored binary and
        // re-acquired, which replaced 2026.8.2.1 with the system's 0.4.51 --
        // older still, and equally missing the feature the check exists to
        // restore. Look before leaping.
        auto candidate = candidate_source_version();
        if (candidate.empty() || !version_is_older(have, candidate)) {
            // stderr, not stdout. This is a remark about the environment,
            // not output of the command that happens to be running -- and
            // `mcpp test --json` promises every stdout line is NDJSON, a
            // promise this line broke the moment a machine fell behind the
            // pin (e2e 155).
            if (!quiet)
                std::println(stderr,
                             "{:>12} vendored xlings {} is older than the "
                             "pinned {}, but no newer source is available "
                             "(keeping it; run `xlings self update`)",
                             "Note", have, pinnedVersion);
            return destBin;
        }
        if (!quiet)
            std::println(stderr,
                         "{:>12} vendored xlings {} -> {} (pinned {})",
                         "Updating", have, candidate, pinnedVersion);
        std::error_code rec;
        std::filesystem::remove(destBin, rec);
        // fall through and re-acquire
    }

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


std::string vendored_xlings_version(const std::filesystem::path& bin) {
    std::error_code ec;
    if (!std::filesystem::exists(bin, ec)) return {};
    auto r = mcpp::platform::process::capture(std::format(
        "{} --version 2>/dev/null", mcpp::platform::shell::quote(bin.string())));
    if (r.exit_code != 0) return {};
    // Output carries ANSI colour; take the first dotted-numeric run.
    std::string out;
    for (std::size_t i = 0; i < r.output.size(); ++i) {
        if (r.output[i] == '\x1b') {              // skip CSI
            while (i < r.output.size() && r.output[i] != 'm') ++i;
            continue;
        }
        out += r.output[i];
    }
    std::size_t i = 0;
    while (i < out.size()) {
        if (std::isdigit(static_cast<unsigned char>(out[i]))) {
            auto j = i;
            while (j < out.size()
                   && (std::isdigit(static_cast<unsigned char>(out[j]))
                       || out[j] == '.')) ++j;
            auto cand = out.substr(i, j - i);
            if (cand.find('.') != std::string::npos) return cand;
            i = j;
        } else ++i;
    }
    return {};
}

bool version_is_older(std::string_view have, std::string_view want) {
    auto parts = [](std::string_view v) {
        std::vector<long long> out;
        std::size_t i = 0;
        while (i <= v.size()) {
            auto dot = v.find('.', i);
            auto seg = v.substr(i, dot == std::string_view::npos
                                       ? std::string_view::npos : dot - i);
            long long n = 0;
            auto [p, e] = std::from_chars(seg.data(), seg.data() + seg.size(), n);
            if (e != std::errc{}) return std::vector<long long>{};
            out.push_back(n);
            if (dot == std::string_view::npos) break;
            i = dot + 1;
        }
        return out;
    };
    auto a = parts(have), b = parts(want);
    if (a.empty() || b.empty()) return false;   // unparseable is not "behind"
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        long long x = i < a.size() ? a[i] : 0;
        long long y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y;
    }
    return false;
}


std::string candidate_source_version() {
    if (const char* e = std::getenv("MCPP_VENDORED_XLINGS"); e && *e) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(e), ec))
            return vendored_xlings_version(std::filesystem::path(e));
    }
    if (auto sys = mcpp::platform::fs::which(
            std::string("xlings") + std::string(mcpp::platform::exe_suffix)))
        return vendored_xlings_version(*sys);
    return {};
}

} // namespace mcpp::fallback
