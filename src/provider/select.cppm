// mcpp.provider.select — the one provider mcpp uses, and the only module
// outside src/provider/xlings/ that names the adapter
// (.github/tools/check_provider_boundary.py). Everything else imports this
// and speaks the port's words (mcpp.provider).

export module mcpp.provider.select;

import std;
import mcpp.provider;


import mcpp.provider.xlings;
import mcpp.provider.xlings.provider;
import mcpp.provider.xlings.subos_info;

export namespace mcpp::provider {

// What a payload path says (its store, what is beside it), asked of no program.
const Layout& layout() { return mcpp::provider::xlings::layout_instance(); }

// The provider at `at`: its program, its home, the project it acts for.
std::unique_ptr<Provider> open(const Location& at) {
    return std::make_unique<mcpp::provider::xlings::XlingsProvider>(at);
}

// The port's questions, each "none" as std::nullopt.
namespace detail {
inline std::optional<std::filesystem::path> some(std::filesystem::path p) {
    if (p.empty()) return std::nullopt;
    return p;
}
} // namespace detail
std::optional<std::filesystem::path> store_of(const std::filesystem::path& path) {
    return detail::some(layout().store_of(path));
}
std::optional<std::filesystem::path> beside(const std::filesystem::path& path, std::string_view name,
                                            std::string_view requiredRel = {}) {
    return detail::some(layout().beside(path, name, requiredRel));
}
std::optional<std::filesystem::path> beside_tool(const std::filesystem::path& path, std::string_view name) {
    return detail::some(layout().beside_tool(path, name));
}
std::optional<std::filesystem::path> beside_binary(const std::filesystem::path& path, std::string_view name,
                                                   std::string_view binaryRel) {
    return detail::some(layout().beside_binary(path, name, binaryRel));
}
std::optional<std::filesystem::path> environment_of(const std::filesystem::path& compiler) {
    return detail::some(layout().environment_of(compiler));
}
std::optional<int> revision_of(const std::filesystem::path& payloadDir) {
    const int r = layout().revision_of(payloadDir);
    if (r < 0) return std::nullopt;
    return r;
}
std::optional<std::filesystem::path> home_store() { return detail::some(layout().home_store()); }
std::optional<std::filesystem::path> in_home_store(std::string_view name, std::string_view requiredRel = {}) {
    return detail::some(layout().in_home_store(name, requiredRel));
}
std::optional<std::string> index_revision(const std::filesystem::path& indexDir) {
    auto r = layout().index_revision(indexDir);
    if (r.empty()) return std::nullopt;
    return r;
}
std::optional<std::filesystem::path> payload(const Provider& p, const PackageRef& ref) {
    return detail::some(p.payload(ref));
}
// The nasm a build may use (the host's first, then the provider's), and the
// one the provider placed.
std::optional<std::filesystem::path> usable_nasm(const Provider& p) { return detail::some(p.usable_nasm()); }
std::optional<std::filesystem::path> placed_nasm(const Provider& p) { return detail::some(p.placed_nasm()); }

// A runtime environment's declared variables, merged into `ambient` (`set`
// fills an absent one, `prepend` goes in front of what is there).
std::vector<std::pair<std::string, std::string>>
resolve_env(const runtime_info::Info& info, const std::filesystem::path& environment,
            const std::function<std::optional<std::string>(std::string_view)>& ambient = {}) {
    return mcpp::provider::xlings::subos::resolve_env(info, environment, ambient);
}

// The provider version this mcpp ships with and requires at least.
inline constexpr std::string_view kPinnedVersion = mcpp::provider::xlings::pinned::kXlingsVersion;
// The pinned tools mcpp provisions through the provider.
inline constexpr std::string_view kPatchelfVersion = mcpp::provider::xlings::pinned::kPatchelfVersion;
inline constexpr std::string_view kNinjaVersion    = mcpp::provider::xlings::pinned::kNinjaVersion;
inline constexpr std::string_view kNasmVersion     = mcpp::provider::xlings::pinned::kNasmVersion;

// The provider's error lines of a failed run, the last `limit`.
std::vector<std::string> error_tail(std::string_view text, std::size_t limit = 20) {
    return mcpp::provider::xlings::stderr_error_tail(text, limit);
}

// A bound on an index refresh, set from `[index] refresh_timeout`.
void set_index_refresh_timeout(std::chrono::seconds timeout) {
    mcpp::provider::xlings::set_index_refresh_timeout(timeout);
}

} // namespace mcpp::provider
