// mcpp.provider — the package provider port: mcpp's own words for what it
// asks of the program that fetches, installs and describes packages
// (.agents/docs/2026-10-16-xlings-provider-port-and-shared-store-design.md §4).
//
//   mcpp (build / toolchain / pm / runtime / doctor / pack / cli)
//         │  import mcpp.provider.select      this port, and the one provider
//         ▼
//   mcpp.provider             types and interfaces (this module)
//   mcpp.provider.xlings      the adapter: xlings' commands, protocol, layout
//
// Nothing outside src/provider/xlings/ names xlings' commands, its NDJSON,
// its home layout or its store naming; .github/tools/check_provider_boundary.py
// makes that a fact. A payload's path is what the provider answers, and it is
// used as answered: never canonicalised (a borrowed payload is a link entry
// whose path is its identity), never removed by mcpp.

export module mcpp.provider;

import std;

export namespace mcpp::provider {

// ─── Where the provider is ──────────────────────────────────────────

// One provider instance: its program and its home, and the project whose
// scope it acts in (empty: the global scope).
struct Location {
    std::filesystem::path binary;
    std::filesystem::path home;
    std::filesystem::path projectDir;
};

// ─── A package address ──────────────────────────────────────────────

// `[<ns>:]<name>[@<version>]`, as a manifest writes it. The namespace
// defaults to `xim`, the provider's own index: `ninja` and `xim:ninja` are
// one package (mcpp.provider.address_set holds the same rule).
struct PackageRef {
    std::string ns;
    std::string name;
    std::string version;   // empty = unpinned; otherwise a version or a requirement
};

PackageRef parse_ref(std::string_view spec) {
    PackageRef r;
    std::string_view rest = spec;
    if (auto colon = rest.find(':'); colon != std::string_view::npos) {
        r.ns = std::string(rest.substr(0, colon));
        rest = rest.substr(colon + 1);
    } else {
        r.ns = "xim";
    }
    if (auto at = rest.find('@'); at != std::string_view::npos) {
        r.name = std::string(rest.substr(0, at));
        r.version = std::string(rest.substr(at + 1));
    } else {
        r.name = std::string(rest);
    }
    return r;
}

std::string to_string(const PackageRef& r) {
    return r.version.empty() ? std::format("{}:{}", r.ns, r.name) : std::format("{}:{}@{}", r.ns, r.name, r.version);
}

// ─── What an operation says while it runs ──────────────────────────

struct ProgressEvent {
    std::string  phase;      // "download", "extract", "configure", ...
    int          percent = 0;
    std::string  message;
};

struct LogEvent {
    std::string  level;      // "debug" | "info" | "warn" | "error"
    std::string  message;
};

// A structured fact the operation reports (`install_targets`, `install_plan`,
// `payload_status`, ...): its kind and its JSON, for the caller to read.
struct DataEvent {
    std::string  dataKind;
    std::string  payloadJson;
};

struct ErrorEvent {
    std::string  code;
    std::string  message;
    std::string  hint;
    bool         recoverable = false;
};

struct ResultEvent {
    int          exitCode = 0;
    std::string  dataJson;
};

using Event = std::variant<ProgressEvent, LogEvent, DataEvent, ErrorEvent, ResultEvent>;

struct EventHandler {
    virtual ~EventHandler() = default;
    virtual void on_progress(const ProgressEvent&) {}
    virtual void on_log     (const LogEvent&)      {}
    virtual void on_data    (const DataEvent&)     {}
    virtual void on_error   (const ErrorEvent&)    {}
    virtual void on_result  (const ResultEvent&)   {}
};

struct CallResult {
    int                          exitCode = 0;
    std::vector<DataEvent>       dataEvents;
    std::optional<ErrorEvent>    error;
    std::string                  resultJson;
    // The provider's own error-level lines, the last few, kept only when the
    // operation failed: the line that names a rejection.
    std::vector<std::string>     stderrTail;
};

// A download in flight, as the provider reports it.
struct BootstrapFile {
    std::string  name;
    double       downloadedBytes = 0;
    double       totalBytes      = 0;
    bool         started         = false;
    bool         finished        = false;
};

struct BootstrapProgress {
    std::vector<BootstrapFile>  files;
    double                      elapsedSec = 0;
};

using BootstrapProgressCallback = std::function<void(const BootstrapProgress&)>;

// ─── Indexes ────────────────────────────────────────────────────────

// A local index, read without touching the network (`mcpp index status`).
struct IndexStatus {
    std::filesystem::path      dir;
    bool                       present = false;
    bool                       fresh = false;
    std::int64_t               ageSeconds = -1;
    std::optional<std::string> rev;     // the index's content identity; opaque
};

// One index a home is seeded with: its name and where it comes from (a git
// URL, an artifact base and its CN half, the source policy).
struct SeedRepo {
    std::string name;
    std::string url;
    std::string artifact;
    std::string source;
    std::string artifactCn;
    // An index mcpp owns (design §5): where its data is and where the
    // payloads it serves go. Empty: the provider's own places.
    std::filesystem::path dir;
    std::filesystem::path store;
};

// How long a completed index sync suppresses the next (a second sync because
// another package is missing cannot help: the first fetched everything).
inline constexpr std::int64_t kIndexRefreshDebounceSeconds = 120;
inline constexpr std::chrono::seconds kDefaultIndexRefreshTimeout{120};

// ─── A project's environment (mcpp.toml [xlings]) ───────────────────

struct ProjectEnv {
    std::vector<std::string>                          deps;
    std::vector<std::pair<std::string, std::string>>  workspace;
    std::string                                       subos;
    bool empty() const { return deps.empty() && workspace.empty() && subos.empty(); }
};

// ─── A runtime environment's description ────────────────────────────

namespace runtime_info {

// The schema this build understands; a higher one is still read.
inline constexpr int kSupportedSchema = 1;

struct EnvDecl {
    std::string var;
    std::string op;      // "set" | "prepend"
    std::string value;   // may name the environment's directory
};

struct Declarer {
    std::string          binding;   // "<name>@<version>"
    std::vector<EnvDecl> decls;
};

struct PackageIdentity {
    std::string namespace_;
    std::string name;
    std::string version;
    std::string source;
};

struct CapabilityProvider {
    std::string     capability;
    PackageIdentity provider;
};

struct RuntimeArtifact {
    std::string           role;
    PackageIdentity       provider;
    std::filesystem::path path;
    std::string           provenance;
    std::string           abi;
    std::string           digest;
    std::string           hostFingerprint;
};

struct Info {
    int                   schema = 0;
    std::string           runtime;    // "glibc@2.39"
    std::string           hostGlibc;  // optional creation-host floor
    std::vector<Declarer> providers;  // sorted by binding
    std::vector<CapabilityProvider> runtimeProviders;
    std::vector<RuntimeArtifact>    runtimeArtifacts;
    bool                  present = false;
    // Non-empty: the caller surfaces it. Never an error.
    std::string           note;
};

// "glibc@2.39" -> "linux-<arch>-glibc": the runtime string says its family.
std::string family_of(std::string_view runtime, std::string_view arch = "x86_64") {
    const auto at   = runtime.find('@');
    const auto name = runtime.substr(0, at == std::string_view::npos ? runtime.size() : at);
    if (name == "glibc")     return std::format("linux-{}-glibc", arch);
    if (name == "musl")      return std::format("linux-{}-musl", arch);
    if (name == "wasi-libc") return "wasm32-wasi";
    if (name == "macos_sdk") return std::format("darwin-{}", arch);
    if (name == "ucrt")      return std::format("windows-{}-ucrt", arch);
    return "unknown";
}

} // namespace runtime_info

// ─── The provider, as mcpp asks of it ───────────────────────────────

// What a provider program is, read by its handshake (design §12).
struct ProviderInfo {
    std::string version;                     // "2026.10.16.1"
    std::string protocol;                    // "1.10"
    std::vector<std::string> capabilities;
    std::vector<std::string> features;       // index_dir, upstreams, payload_status, ...
    std::vector<int> homeLayouts;
    bool answered = false;                   // false: an older provider without the handshake
};

// A read-only home this provider installs from by link (design 2026-10-16
// §8): its home, its store (empty: the home's), the namespaces taken from it
// (empty: every one).
struct Upstream {
    std::filesystem::path home;
    std::filesystem::path store;
    std::vector<std::string> namespaces;
};

// A payload installed here that an upstream has with the same identity
// (namespace, name, version, revision) and complete: the copy here could be
// a link to it (`mcpp self store dedupe`, §8.5).
struct Duplicate {
    std::filesystem::path local;
    std::filesystem::path upstream;
};

// One payload in a store: its package and version, and its directory.
struct StoreEntry {
    std::string ns;
    std::string name;
    std::string version;
    std::filesystem::path dir;
};

// What a payload path says without asking the provider program: the store
// it is in and what is beside it. Each answer is a path as the store spells
// it, never canonicalised; an empty path is "none" (std::optional and
// std::expected<void, ...> are kept out of the virtual signatures of this
// module: GCC 16 writes an unreadable module for its importers when an
// abstract class returns one).
class Layout {
public:
    virtual ~Layout() = default;
    // The store `path` (a payload, or a file inside one) is in.
    virtual std::filesystem::path store_of(const std::filesystem::path& path) const = 0;
    // A version of `name` (any namespace) beside `path`'s payload, holding
    // something (and `requiredRel`, when given).
    virtual std::filesystem::path beside(const std::filesystem::path& path, std::string_view name,
                                         std::string_view requiredRel) const = 0;
    // A version directory of the provider-index tool `name` beside `path`.
    virtual std::filesystem::path beside_tool(const std::filesystem::path& path, std::string_view name) const = 0;
    // `binaryRel` inside a version of the provider-index tool `name` beside `path`.
    virtual std::filesystem::path beside_binary(const std::filesystem::path& path, std::string_view name,
                                                std::string_view binaryRel) const = 0;
    // The runtime environment a toolchain at `compiler` belongs to.
    virtual std::filesystem::path environment_of(const std::filesystem::path& compiler) const = 0;
    // The packaging revision recorded in a payload; -1: no record.
    virtual int revision_of(const std::filesystem::path& payloadDir) const = 0;
    // The store of mcpp's own home, and a package found in it.
    virtual std::filesystem::path home_store() const = 0;
    virtual std::filesystem::path in_home_store(std::string_view name, std::string_view requiredRel) const = 0;
    // The store under a provider's data root (a project's).
    virtual std::filesystem::path store_in(const std::filesystem::path& dataRoot) const = 0;
    // A directory of `store` that holds the package under an older naming
    // (the namespaces `prefixes` allow); empty: none.
    virtual std::string legacy_install_dir(const std::filesystem::path& store, std::string_view qualifiedName,
                                           std::string_view shortName,
                                           const std::vector<std::string>& prefixes) const = 0;
    // The store of a home.
    virtual std::filesystem::path store_of_home(const std::filesystem::path& home) const = 0;
    // The home a store belongs to.
    virtual std::filesystem::path home_of_store(const std::filesystem::path& store) const = 0;
    // Whether `dir` is a home's default runtime environment.
    virtual bool is_default_environment(std::string_view dir) const = 0;
    // Whether `dir` is a store (a payload's path is <store>/...).
    virtual bool is_store(const std::filesystem::path& dir) const = 0;
    // The directory of every version of the provider-index package `name`
    // in `store`.
    virtual std::filesystem::path tool_in(const std::filesystem::path& store, std::string_view name) const = 0;
    // Every payload in `store`, and the payload a path is in (empty name: none).
    virtual std::vector<StoreEntry> entries(const std::filesystem::path& store) const = 0;
    virtual StoreEntry entry_of(const std::filesystem::path& path) const = 0;
    // What a path of a provider home is: "payload", "environment", "index",
    // or "" (nothing the provider keeps).
    virtual std::string kind_of(const std::filesystem::path& path) const = 0;
    // A runtime environment of a home ("default"), a project's provider
    // home, and the file an environment describes itself in (its changes
    // mark the environment changed).
    virtual std::filesystem::path environment(const std::filesystem::path& home, std::string_view name) const = 0;
    virtual std::filesystem::path project_home(const std::filesystem::path& projectRoot) const = 0;
    virtual std::filesystem::path description(const std::filesystem::path& environment) const = 0;
    // The index directories under a data root, whether any index has data
    // there, and where a provider-index package's descriptor may be (in the
    // order an index is consulted).
    virtual std::vector<std::filesystem::path> index_dirs(const std::filesystem::path& dataRoot) const = 0;
    virtual bool index_data_present(const std::filesystem::path& dataRoot) const = 0;
    virtual std::vector<std::filesystem::path> descriptor_candidates(const std::filesystem::path& dataRoot,
                                                                     std::string_view name) const = 0;
    // A runtime environment's description of itself.
    virtual runtime_info::Info runtime_info(const std::filesystem::path& environment) const = 0;
    // An index's content identity (opaque: compared and printed, never
    // parsed); empty: none.
    virtual std::string index_revision(const std::filesystem::path& indexDir) const = 0;
    // A provider home's index directories and stores (the global home, or a
    // project's: project_home), read from its configuration alone.
    virtual std::vector<std::filesystem::path> index_roots_of(const std::filesystem::path& home) const = 0;
    virtual std::vector<std::filesystem::path> stores_of(const std::filesystem::path& home) const = 0;
    // The indexes a provider home is configured to serve, its default
    // included; empty: the directory is not a provider home (no
    // configuration there).
    virtual std::vector<std::string> indices_of(const std::filesystem::path& home) const = 0;
    // The store an mcpp home (or a project's .mcpp) keeps the payloads of
    // mcpp's own indices in: the provider names a store by its last
    // component (design §5.1).
    virtual std::filesystem::path own_store(const std::filesystem::path& mcppHome) const = 0;
    // What a directory offers as an upstream: an mcpp home (its registry for
    // the provider's own packages, its store for mcpp's), a provider home (its
    // own packages only), or nothing (empty).
    virtual std::vector<Upstream> upstream_of(const std::filesystem::path& dir) const = 0;
    // Whether `name` is one of the provider's own indexes (its default and
    // the ones it adds itself), which stay where the provider keeps them.
    virtual bool is_provider_index(std::string_view name) const = 0;
};

// What a provider says of a package's payload without installing anything
// (its `payload_status`, design §7.1 / X4): where it is, whether it is
// complete (there, holding something, no recorded failure, the recipe's
// revision), and whether it is this home's own or an upstream's link entry.
struct PayloadStatus {
    std::string request;            // the address as asked
    std::string ns, name, version;  // what it resolved to
    int revision = 0;               // the recipe's
    std::filesystem::path dir;      // the payload directory (empty: does not resolve)
    std::string origin;             // "local" | "upstream" | "absent"
    int installedRevision = -1;     // what the payload's stamp records; -1: none
    bool complete = false;
    std::string error;              // non-empty: the request did not resolve
};

// One provider at one Location.
class Provider {
public:
    virtual ~Provider() = default;
    virtual const Location& location() const = 0;

    // ── payloads ──
    // Where an installed payload of `ref` is (the provider's own answer
    // first, then its version grammar over what is installed).
    virtual std::filesystem::path payload(const PackageRef& ref) const = 0;   // empty: not installed
    // A pinned tool's payload directory, and the directory of all its versions.
    virtual std::filesystem::path pinned(std::string_view tool, std::string_view version) const = 0;
    virtual std::filesystem::path versions(std::string_view tool) const = 0;
    // Remember what an install resolved each request to (its `install_targets`).
    virtual void remember(const DataEvent& installTargets) const = 0;
    // The store this provider installs into, and every store a payload here
    // may be in (the indices' own, then the home's).
    virtual std::filesystem::path store() const = 0;
    virtual std::vector<std::filesystem::path> stores() const = 0;
    // What each request resolves to and what its payload is here, nothing
    // installed; one entry per request, in order (empty: the provider cannot
    // say -- a provider before the handshake).
    virtual std::vector<PayloadStatus> status(std::span<const PackageRef> refs) const = 0;

    // ── the environment it keeps ──
    virtual std::filesystem::path sysroot() const = 0;     // headers and libraries a build targets
    virtual std::filesystem::path bin_dir() const = 0;     // the programs it placed
    virtual bool initialized() const = 0;
    virtual std::filesystem::path data_root() const = 0;   // where its indexes are
    virtual std::filesystem::path config_file() const = 0; // the home's own configuration

    // ── operations ──
    virtual std::expected<CallResult, std::string>
        call(std::string_view operation, std::string_view argsJson, EventHandler* handler = nullptr) const = 0;
    virtual int install_with_progress(std::string_view target, const BootstrapProgressCallback& cb,
                                      bool quiet = false) const = 0;
    virtual int install_direct(std::string_view target, bool quiet = false) const = 0;
    // The command line that runs the provider program here (diagnostics and
    // the few commands a person is told to run).
    virtual std::string command_prefix() const = 0;
    virtual std::string operation_command(std::string_view operation, std::string_view argsJson) const = 0;

    // ── setup ──
    virtual void seed(std::span<const SeedRepo> repos, std::string_view mirror = "auto",
                      const ProjectEnv& penv = {}) const = 0;
    // The upstreams this home installs from (replacing what it had; none:
    // self-contained). Never written to.
    virtual void set_upstreams(std::span<const Upstream> upstreams) const = 0;
    // What this home installed itself that an upstream has the same of, and
    // one of them replaced by a link to the upstream's (atomically; empty:
    // done, else why not).
    virtual std::vector<Duplicate> duplicates() const = 0;
    virtual std::string link_to_upstream(const Duplicate& d) const = 0;
    virtual int show_config() const = 0;
    virtual int set_mirror(std::string_view mirror, bool quiet = false) const = 0;
    virtual void ensure_initialized(bool quiet) const = 0;
    // The runtime the toolchains this provider installs run on (design
    // 2026-10-16 §7.2): mcpp's selected one ("glibc@2.44.3"), pinned once.
    // Pinning makes the provider's links to it current; `relink` makes them
    // again when the pin already says so (its payload was installed since).
    virtual void pin_runtime(std::string_view runtimeId, bool relink) const = 0;
    virtual void ensure_patchelf(bool quiet, const BootstrapProgressCallback& cb) const = 0;
    virtual void ensure_ninja(bool quiet, const BootstrapProgressCallback& cb) const = 0;
    virtual std::filesystem::path usable_nasm() const = 0;   // empty: none
    virtual std::filesystem::path placed_nasm() const = 0;
    virtual bool nasm_is_from_host(const std::filesystem::path& bin) const = 0;

    // ── indexes ──
    // Every directory an index of this scope keeps its data in, sorted.
    virtual std::vector<std::filesystem::path> index_roots() const = 0;
    // Take over the indices mcpp owns (`dir`, `store` set): their places
    // recorded in the provider's configuration, data the provider kept at its
    // own place moved there once.
    virtual void own_indices(std::span<const SeedRepo> owned) const = 0;
    virtual IndexStatus default_index_status(std::int64_t ttlSeconds) const = 0;
    virtual IndexStatus official_index_status(std::int64_t ttlSeconds) const = 0;
    virtual bool index_fresh(std::int64_t ttlSeconds) const = 0;
    virtual bool official_index_fresh(std::int64_t ttlSeconds) const = 0;
    virtual bool official_package_index_fresh(std::string_view package, std::int64_t ttlSeconds) const = 0;
    virtual bool official_package_present(std::string_view package) const = 0;
    virtual int update_index(bool quiet = false) const = 0;
    virtual void ensure_index_fresh(std::int64_t ttlSeconds, bool quiet = false) const = 0;
};

} // namespace mcpp::provider
