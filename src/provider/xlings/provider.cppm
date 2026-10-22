// mcpp.provider.xlings.provider — xlings as mcpp's package provider: the
// port's Layout and Provider over the adapter's commands and layout
// (mcpp.provider.xlings, mcpp.provider.xlings.subos_info).

export module mcpp.provider.xlings.provider;

import std;
import mcpp.provider;
import mcpp.provider.xlings;
import mcpp.provider.xlings.subos_info;
import mcpp.provider.xlings.legacy_dirs;
import mcpp.libs.json;
import mcpp.platform;

export namespace mcpp::provider::xlings {

class XlingsLayout final : public mcpp::provider::Layout {
public:
    std::filesystem::path store_of(const std::filesystem::path& path) const override {
        return paths::xpkgs_from_compiler(path).value_or(std::filesystem::path{});
    }
    std::filesystem::path beside(const std::filesystem::path& path, std::string_view name,
                                 std::string_view requiredRel) const override {
        return paths::find_sibling_package(path, name, requiredRel).value_or(std::filesystem::path{});
    }
    std::filesystem::path beside_tool(const std::filesystem::path& path, std::string_view name) const override {
        return paths::find_sibling_tool(path, name).value_or(std::filesystem::path{});
    }
    std::filesystem::path beside_binary(const std::filesystem::path& path, std::string_view name,
                                        std::string_view binaryRel) const override {
        return paths::find_sibling_binary(path, name, binaryRel).value_or(std::filesystem::path{});
    }
    std::filesystem::path environment_of(const std::filesystem::path& compiler) const override {
        return paths::subos_dir_of(compiler).value_or(std::filesystem::path{});
    }
    int revision_of(const std::filesystem::path& payloadDir) const override {
        return paths::installed_revision(payloadDir).value_or(-1);
    }
    std::filesystem::path home_store() const override {
        return paths::active_home_xpkgs().value_or(std::filesystem::path{});
    }
    std::filesystem::path in_home_store(std::string_view name, std::string_view requiredRel) const override {
        return paths::find_home_tool(name, requiredRel).value_or(std::filesystem::path{});
    }
    std::filesystem::path store_in(const std::filesystem::path& dataRoot) const override { return dataRoot / "xpkgs"; }
    std::string legacy_install_dir(const std::filesystem::path& store, std::string_view qualifiedName,
                                   std::string_view shortName,
                                   const std::vector<std::string>& prefixes) const override {
        return scan_legacy_install_dirs(store, qualifiedName, shortName, prefixes).value_or(std::string{});
    }
    std::filesystem::path store_of_home(const std::filesystem::path& home) const override {
        return home / "data" / "xpkgs";
    }
    std::filesystem::path home_of_store(const std::filesystem::path& store) const override {
        return store.parent_path().parent_path();
    }
    bool is_default_environment(std::string_view dir) const override { return dir.ends_with("subos/default"); }
    bool is_store(const std::filesystem::path& dir) const override {
        return dir.filename() == "xpkgs" && dir.parent_path().filename() == "data";
    }
    std::filesystem::path tool_in(const std::filesystem::path& store, std::string_view name) const override {
        return store / std::format("xim-x-{}", name);
    }
    std::vector<mcpp::provider::StoreEntry> entries(const std::filesystem::path& store) const override {
        std::vector<mcpp::provider::StoreEntry> out;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(store, ec); !ec && it != std::filesystem::directory_iterator{};
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const auto pkg = it->path().filename().string();
            const auto sep = pkg.find("-x-");
            if (sep == std::string::npos) continue;
            std::error_code vec;
            for (auto v = std::filesystem::directory_iterator(it->path(), vec); !vec && v != std::filesystem::directory_iterator{};
                 v.increment(vec)) {
                if (!v->is_directory(vec)) continue;
                const auto ver = v->path().filename().string();
                if (ver.empty() || ver.front() == '.') continue;
                out.push_back({pkg.substr(0, sep), pkg.substr(sep + 3), ver, v->path()});
            }
        }
        return out;
    }
    mcpp::provider::StoreEntry entry_of(const std::filesystem::path& path) const override {
        // <store>/<ns>-x-<name>/<version>/...: the chain of ancestors, leaf first.
        std::vector<std::filesystem::path> chain;
        for (auto p = path;; p = p.parent_path()) {
            chain.push_back(p);
            if (!p.has_parent_path() || p.parent_path() == p) break;
        }
        for (std::size_t i = 1; i < chain.size(); ++i) {
            if (chain[i].filename() != "xpkgs") continue;
            const auto pkg = chain[i - 1].filename().string();
            const auto sep = pkg.find("-x-");
            if (sep == std::string::npos) return {};
            mcpp::provider::StoreEntry e{pkg.substr(0, sep), pkg.substr(sep + 3), {}, {}};
            if (i >= 2) {
                e.version = chain[i - 2].filename().string();
                e.dir = chain[i - 2];
            }
            return e;
        }
        return {};
    }
    std::string kind_of(const std::filesystem::path& path) const override {
        const auto s = path.generic_string();
        if (s.find("registry/data/xpkgs") != std::string::npos) return "payload";
        if (s.find("registry/subos") != std::string::npos) return "environment";
        if (s.find("registry/data/") != std::string::npos) return "index";
        return {};
    }
    std::filesystem::path environment(const std::filesystem::path& home, std::string_view name) const override {
        return home / "subos" / std::string(name);
    }
    std::filesystem::path project_home(const std::filesystem::path& projectRoot) const override {
        return projectRoot / ".mcpp" / ".xlings";
    }
    std::filesystem::path description(const std::filesystem::path& environment) const override {
        return environment / ".xlings.json";
    }
    std::vector<std::filesystem::path> index_dirs(const std::filesystem::path& dataRoot) const override {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dataRoot, ec); !ec && it != std::filesystem::directory_iterator{};
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const auto n = it->path().filename().string();
            if (n == "xpkgs" || n == "runtimedir" || n == "stale" || n.starts_with(".")) continue;
            if (n == "xim-index-repos") {
                std::error_code rec;
                for (auto r = std::filesystem::directory_iterator(it->path(), rec);
                     !rec && r != std::filesystem::directory_iterator{}; r.increment(rec))
                    if (r->is_directory(rec)) out.push_back(r->path());
                continue;
            }
            out.push_back(it->path());
        }
        std::ranges::sort(out);
        return out;
    }
    bool index_data_present(const std::filesystem::path& data) const override {
        std::error_code ec;
        auto reposDir = data / "xim-index-repos";
        if (std::filesystem::exists(reposDir / "xim-indexrepos.json", ec)) return true;
        for (const auto& base : {reposDir, data}) {
            if (!std::filesystem::is_directory(base, ec)) continue;
            for (auto const& entry : std::filesystem::directory_iterator(base, ec))
                if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "pkgs", ec)) return true;
        }
        return false;
    }
    std::vector<std::filesystem::path> descriptor_candidates(const std::filesystem::path& data,
                                                             std::string_view name) const override {
        std::vector<std::filesystem::path> out;
        if (name.empty()) return out;
        const std::string subdir(1, name[0]), fname = std::string(name) + ".lua";
        // xim-pkgindex first, then each sub-index.
        out.push_back(data / "xim-pkgindex" / "pkgs" / subdir / fname);
        std::error_code ec;
        std::vector<std::filesystem::path> repos;
        for (auto it = std::filesystem::directory_iterator(data / "xim-index-repos", ec);
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
            if (it->is_directory(ec)) repos.push_back(it->path());
        std::ranges::sort(repos);
        for (const auto& r : repos) out.push_back(r / "pkgs" / subdir / fname);
        return out;
    }
    mcpp::provider::runtime_info::Info runtime_info(const std::filesystem::path& environment) const override {
        return subos::read(environment);
    }
    std::string index_revision(const std::filesystem::path& indexDir) const override {
        return xlings::index_revision(indexDir).value_or(std::string{});
    }
    std::vector<std::filesystem::path> index_roots_of(const std::filesystem::path& home) const override {
        return paths::index_roots_of(home);
    }
    std::vector<std::filesystem::path> stores_of(const std::filesystem::path& home) const override {
        return paths::stores_of(home);
    }
    std::vector<std::string> indices_of(const std::filesystem::path& home) const override {
        return paths::indices_of(home);
    }
    std::filesystem::path own_store(const std::filesystem::path& mcppHome) const override {
        // xlings takes a store only by the name `xpkgs` (a payload's path is
        // its identity, <store>/<ns>-x-<name>/<version>).
        return mcppHome / "xpkgs";
    }
    std::vector<mcpp::provider::Upstream> upstream_of(const std::filesystem::path& dir) const override {
        std::error_code ec;
        // An mcpp home: the provider home under it serves xim, its store the
        // namespaces of mcpp's indices -- never xim from a plain xlings home's
        // view of mcpp's packages (design §8.3).
        if (std::filesystem::is_regular_file(dir / "registry" / ".xlings.json", ec))
            return {{dir / "registry", {}, {"xim"}}, {dir / "registry", own_store(dir), {}}};
        if (std::filesystem::is_regular_file(dir / ".xlings.json", ec))
            return {{dir, {}, {"xim"}}};
        return {};
    }
    // A Luban system (xlings design 2026-10-21 §3.7): its os-release says
    // ID=luban and /usr/lib/xlings-abi is the runtime xlings bound; its root
    // record (/etc/xlings/usr) names its generation, <home>/subos/<name>/root/usr
    // -- /xlings for one installed on a machine, the path of the home that
    // made it for one made on a host.
    bool declared_system_root(const std::filesystem::path& root) const override {
        std::error_code ec;
        if (!std::filesystem::exists(root / "usr/lib/xlings-abi", ec)) return false;
        std::ifstream in(root / "etc/os-release");
        for (std::string line; std::getline(in, line);) {
            if (!line.starts_with("ID=")) continue;
            auto id = line.substr(3);
            if (id.size() >= 2 && (id.front() == '"' || id.front() == '\'')) id = id.substr(1, id.size() - 2);
            return id == "luban";
        }
        return false;
    }
    std::filesystem::path system_home(const std::filesystem::path& root) const override {
        if (!declared_system_root(root)) return {};
        std::ifstream in(root / "etc/xlings/usr");
        std::string line;
        if (!std::getline(in, line)) return {};
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::filesystem::path usr = std::filesystem::path(line).lexically_normal();
        const auto rootDir = usr.parent_path(), instance = rootDir.parent_path(), subos = instance.parent_path();
        if (!usr.is_absolute() || usr.filename() != "usr" || rootDir.filename() != "root" || subos.filename() != "subos") return {};
        std::error_code ec;
        const auto home = subos.parent_path();
        return std::filesystem::is_directory(home, ec) ? home : std::filesystem::path{};
    }
    bool is_provider_index(std::string_view name) const override {
        // xlings' default index and the ones it adds beside it.
        return name == "xim" || name == "awesome" || name == "scode" || name == "d2x";
    }
};

class XlingsProvider final : public mcpp::provider::Provider {
public:
    explicit XlingsProvider(mcpp::provider::Location at) : env_(std::move(at)) {}
    const mcpp::provider::Location& location() const override { return env_; }

    std::filesystem::path payload(const mcpp::provider::PackageRef& ref) const override {
        return paths::xpkg_payload(env_, ref).value_or(std::filesystem::path{});
    }
    std::filesystem::path pinned(std::string_view tool, std::string_view version) const override {
        return paths::xim_tool(env_, tool, version);
    }
    std::filesystem::path versions(std::string_view tool) const override { return paths::xim_tool_root(env_, tool); }
    void remember(const mcpp::provider::DataEvent& installTargets) const override {
        paths::record_resolutions(env_, paths::parse_install_targets(installTargets.payloadJson));
    }

    std::filesystem::path store() const override { return paths::xpkgs_base(env_); }
    std::vector<std::filesystem::path> stores() const override { return paths::stores(env_); }
    std::vector<mcpp::provider::PayloadStatus> status(std::span<const mcpp::provider::PackageRef> refs) const override {
        std::vector<mcpp::provider::PayloadStatus> out;
        if (refs.empty()) return out;
        nlohmann::json args{{"targets", nlohmann::json::array()}};
        for (const auto& r : refs) args["targets"].push_back(mcpp::provider::to_string(r));
        auto r = xlings::call(env_, "payload_status", args.dump(), nullptr);
        if (!r || r->exitCode != 0) return out;
        for (const auto& d : r->dataEvents) {
            if (d.dataKind != "payload_status") continue;
            auto j = nlohmann::json::parse(d.payloadJson, nullptr, false);
            if (!j.is_object() || !j.contains("targets") || !j["targets"].is_array()) continue;
            const auto str = [](const nlohmann::json& o, const char* k) {
                return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string{};
            };
            for (const auto& t : j["targets"]) {
                mcpp::provider::PayloadStatus ps;
                ps.request = str(t, "request");
                ps.ns = str(t, "namespace");
                ps.name = str(t, "name");
                ps.version = str(t, "version");
                if (t.contains("revision") && t["revision"].is_number_integer()) ps.revision = t["revision"].get<int>();
                ps.dir = str(t, "payload_dir");
                ps.origin = str(t, "origin");
                if (t.contains("installed_revision") && t["installed_revision"].is_number_integer())
                    ps.installedRevision = t["installed_revision"].get<int>();
                ps.complete = t.value("complete", false);
                if (t.contains("floor") && t["floor"].is_number_integer()) ps.floor = t["floor"].get<int>();
                // COMPAT(internal:provider-protocol-1.10): an xlings before
                // 2026.10.17.1 reports no `usable` / `current`, and its
                // `complete` already required the recipe's revision -- so it
                // is both, read as 1.10 meant it. A vendored xlings that cannot
                // be updated (no newer source beside this mcpp) keeps the
                // behaviour it had.
                ps.usable = t.contains("usable") ? t.value("usable", false) : ps.complete;
                ps.current = t.contains("current") ? t.value("current", false) : ps.complete;
                ps.error = str(t, "error");
                out.push_back(std::move(ps));
            }
        }
        if (out.size() != refs.size()) out.clear();
        return out;
    }
    std::filesystem::path sysroot() const override { return paths::sysroot(env_); }
    std::filesystem::path bin_dir() const override { return paths::sandbox_bin(env_); }
    bool initialized() const override {
        std::error_code ec;
        return std::filesystem::exists(paths::sandbox_init_marker(env_), ec);
    }
    std::filesystem::path data_root() const override { return paths::index_data(env_); }
    std::filesystem::path config_file() const override {
        return (env_.projectDir.empty() ? env_.home : env_.projectDir) / ".xlings.json";
    }

    std::expected<mcpp::provider::CallResult, std::string>
    call(std::string_view operation, std::string_view argsJson, mcpp::provider::EventHandler* handler) const override {
        return xlings::call(env_, operation, argsJson, handler);
    }
    int install_with_progress(std::string_view target, const mcpp::provider::BootstrapProgressCallback& cb,
                              bool quiet) const override {
        return xlings::install_with_progress(env_, target, cb, quiet);
    }
    int install_direct(std::string_view target, bool quiet) const override {
        return xlings::install_direct(env_, target, quiet);
    }
    std::string command_prefix() const override { return build_command_prefix(env_); }
    std::string operation_command(std::string_view operation, std::string_view argsJson) const override {
        return build_interface_command(env_, operation, argsJson);
    }

    void seed(std::span<const mcpp::provider::SeedRepo> repos, std::string_view mirror,
              const mcpp::provider::ProjectEnv& penv) const override {
        seed_xlings_json(env_, repos, mirror, penv);
    }
    void set_upstreams(std::span<const mcpp::provider::Upstream> upstreams) const override {
        const auto file = env_.home / ".xlings.json";
        std::ifstream in(file);
        if (!in) return;
        auto j = nlohmann::json::parse(in, nullptr, false);
        in.close();
        if (!j.is_object()) return;
        nlohmann::json list = nlohmann::json::array();
        for (const auto& u : upstreams) {
            nlohmann::json e{{"home", u.home.generic_string()}};
            if (!u.store.empty()) e["store"] = u.store.generic_string();
            if (!u.namespaces.empty()) e["namespaces"] = u.namespaces;
            list.push_back(std::move(e));
        }
        if (list.empty() ? !j.contains("upstreams") : (j.contains("upstreams") && j["upstreams"] == list)) return;
        if (list.empty()) j.erase("upstreams");
        else j["upstreams"] = std::move(list);
        auto tmp = file;
        tmp += std::format(".mcpp-{:x}", std::random_device{}());
        { std::ofstream(tmp) << j.dump(2) << "\n"; }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }
    std::vector<mcpp::provider::Duplicate> duplicates() const override {
        std::vector<mcpp::provider::Duplicate> out;
        std::ifstream in(env_.home / ".xlings.json");
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains("upstreams") || !j["upstreams"].is_array()) return out;
        std::error_code ec;
        for (const auto& store : paths::stores(env_)) {
            for (const auto& entry : XlingsLayout{}.entries(store)) {
                if (std::filesystem::is_symlink(entry.dir, ec) || std::filesystem::is_empty(entry.dir, ec)) continue;
                const auto mine = paths::installed_revision(entry.dir);
                if (!mine) continue;
                for (const auto& u : j["upstreams"]) {
                    if (!u.is_object() || !u.contains("home") || !u["home"].is_string()) continue;
                    if (u.contains("namespaces") && u["namespaces"].is_array() && !u["namespaces"].empty()
                        && std::ranges::none_of(u["namespaces"], [&](const nlohmann::json& n) { return n == entry.ns; }))
                        continue;
                    const std::filesystem::path home = u["home"].get<std::string>();
                    const std::filesystem::path ustore = u.contains("store") && u["store"].is_string()
                        ? std::filesystem::path(u["store"].get<std::string>()) : home / "data" / "xpkgs";
                    const auto candidate = ustore / entry.dir.parent_path().filename() / entry.dir.filename();
                    if (!std::filesystem::is_directory(candidate, ec) || std::filesystem::is_empty(candidate, ec)) continue;
                    if (paths::installed_revision(candidate) != mine) continue;
                    out.push_back({entry.dir, candidate});
                    break;
                }
            }
        }
        return out;
    }
    std::string link_to_upstream(const mcpp::provider::Duplicate& d) const override {
        std::error_code ec;
        auto link = d.local;
        link += std::format(".mcpp-link-{:x}", std::random_device{}());
        if (!mcpp::platform::create_directory_link(d.upstream, link))
            return std::format("cannot link {} to {}", link.string(), d.upstream.string());
        if (auto swapped = mcpp::platform::exchange_paths(link, d.local); !swapped) {
            std::filesystem::remove(link, ec);
            return swapped.error();
        }
        // The copy this home installed, now at the scratch name.
        std::filesystem::remove_all(link, ec);
        return {};
    }
    int show_config() const override { return config_show(env_); }
    int set_mirror(std::string_view mirror, bool quiet) const override { return config_set_mirror(env_, mirror, quiet); }
    mcpp::provider::NetworkReport check_network(std::chrono::milliseconds budget) const override {
        return network_check(env_, budget);
    }
    void ensure_initialized(bool quiet) const override { ensure_init(env_, quiet); }
    void pin_runtime(std::string_view runtimeId, bool relink) const override {
        // `.xlings.json` `runtime: {"glibc": "2.44.3"}` is the pin; set
        // through xlings itself, and only when it says otherwise or the
        // links must be made again: `config --runtime` makes <home>/abi
        // (the loader a toolchain names) point at the pinned payload, and
        // installing that payload alone does not.
        const auto at = runtimeId.find('@');
        if (at == std::string_view::npos) return;
        const std::string family(runtimeId.substr(0, at)), version(runtimeId.substr(at + 1));
        std::ifstream in(env_.home / ".xlings.json");
        auto j = nlohmann::json::parse(in, nullptr, false);
        std::error_code ec;
        if (!relink && std::filesystem::exists(std::filesystem::symlink_status(env_.home / "abi", ec))
            && j.is_object() && j.contains("runtime") && j["runtime"].is_object()
            && j["runtime"].value(family, std::string{}) == version)
            return;
        mcpp::provider::Location global = env_;
        global.projectDir.clear();
        (void)run_capture(build_command_prefix(global) + " config --runtime "
                          + mcpp::platform::shell::quote(runtimeId) + " "
                          + std::string(mcpp::platform::shell::silent_redirect));
    }
    void ensure_patchelf(bool quiet, const mcpp::provider::BootstrapProgressCallback& cb) const override {
        xlings::ensure_patchelf(env_, quiet, cb);
    }
    void ensure_ninja(bool quiet, const mcpp::provider::BootstrapProgressCallback& cb) const override {
        xlings::ensure_ninja(env_, quiet, cb);
    }
    std::filesystem::path usable_nasm() const override { return find_usable_nasm(env_).value_or(std::filesystem::path{}); }
    std::filesystem::path placed_nasm() const override { return find_sandbox_nasm(env_).value_or(std::filesystem::path{}); }
    bool nasm_is_from_host(const std::filesystem::path& bin) const override { return xlings::nasm_is_from_host(env_, bin); }

    std::vector<std::filesystem::path> index_roots() const override { return paths::index_roots(env_); }
    void own_indices(std::span<const mcpp::provider::SeedRepo> owned) const override {
        paths::own_indices(env_, owned);
    }
    mcpp::provider::IndexStatus default_index_status(std::int64_t ttlSeconds) const override {
        return xlings::default_index_status(env_, ttlSeconds);
    }
    mcpp::provider::IndexStatus official_index_status(std::int64_t ttlSeconds) const override {
        return xlings::official_index_status(env_, ttlSeconds);
    }
    bool index_fresh(std::int64_t ttlSeconds) const override { return is_index_fresh(env_, ttlSeconds); }
    bool official_index_fresh(std::int64_t ttlSeconds) const override { return is_official_index_fresh(env_, ttlSeconds); }
    bool official_package_index_fresh(std::string_view package, std::int64_t ttlSeconds) const override {
        return is_official_package_index_fresh(env_, package, ttlSeconds);
    }
    bool official_package_present(std::string_view package) const override {
        return xlings::official_package_present(env_, package);
    }
    int update_index(bool quiet) const override { return xlings::update_index(env_, quiet); }
    void ensure_index_fresh(std::int64_t ttlSeconds, bool quiet) const override {
        xlings::ensure_index_fresh(env_, ttlSeconds, quiet);
    }

private:
    mcpp::provider::Location env_;
};

const mcpp::provider::Layout& layout_instance() {
    static const XlingsLayout instance;
    return instance;
}

} // namespace mcpp::provider::xlings
