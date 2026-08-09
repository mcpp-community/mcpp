// mcpp.scaffold — package-based project templates (design v2).
//
// Mechanism owned by mcpp: SPEC grammar, rendering, dependency injection.
// Vocabulary owned by packages: template names, contents, default choice
// (closed grammar / open vocabulary applied to scaffolding).
//
// A package opts in by shipping a `templates/` directory:
//   templates/<name>/template.toml   — metadata (description/default/inject)
//   templates/<name>/**.in           — rendered ({{var}} placeholders)
//   templates/<name>/** (non-.in)    — copied verbatim
//
// Trust boundary: templates are pure data — render + copy, no hooks and no
// script execution.

export module mcpp.scaffold;

import std;
import mcpp.libs.toml;
import mcpp.manifest;
import mcpp.platform.scaffold_fs;
import mcpp.pm.dependency_selector;

export namespace mcpp::scaffold {

// `--template` package-form SPEC:
//   [ns.]pkg | [ns.]pkg:tmpl | [ns.]pkg@ver | [ns.]pkg@ver:tmpl
// A trailing ':' is accepted for one migration release as the old listing
// alias; the canonical surface is `--list-templates [ns.]pkg[@ver]`.
struct TemplateSpec {
    mcpp::pm::PackageSelector package;
    std::optional<std::string> version;
    std::optional<std::string> templateName;
    bool                       legacyList = false;
};

struct TemplateSpecError {
    std::string message;
};

std::expected<TemplateSpec, TemplateSpecError>
parse_template_spec(std::string_view spelling) {
    std::string_view packageAndVersion = spelling;
    std::optional<std::string> templateName;
    bool legacyList = false;

    // Delimiters are parsed outside-in in one fixed order. A second delimiter
    // is always an error; it is never reinterpreted as package/version text.
    if (auto colon = spelling.find(':'); colon != std::string_view::npos) {
        if (spelling.find(':', colon + 1) != std::string_view::npos) {
            return std::unexpected(TemplateSpecError{std::format(
                "invalid template spec '{}': multiple ':' delimiters",
                spelling)});
        }
        packageAndVersion = spelling.substr(0, colon);
        auto requested = spelling.substr(colon + 1);
        if (requested.empty()) {
            legacyList = true;
        } else {
            auto parsedName = mcpp::pm::parse_package_selector(requested);
            if (!parsedName || parsedName->namespace_) {
                return std::unexpected(TemplateSpecError{std::format(
                    "invalid template spec '{}': template name '{}' must be "
                    "one atom containing only ASCII letters, digits, '-' or '_'",
                    spelling, requested)});
            }
            templateName = std::string(requested);
        }
    }

    std::string_view packageSpelling = packageAndVersion;
    std::optional<std::string> version;
    if (auto at = packageAndVersion.find('@'); at != std::string_view::npos) {
        if (packageAndVersion.find('@', at + 1) != std::string_view::npos) {
            return std::unexpected(TemplateSpecError{std::format(
                "invalid template spec '{}': multiple '@' delimiters",
                spelling)});
        }
        packageSpelling = packageAndVersion.substr(0, at);
        auto exact = packageAndVersion.substr(at + 1);
        if (exact.empty()) {
            return std::unexpected(TemplateSpecError{std::format(
                "invalid template spec '{}': version after '@' is empty",
                spelling)});
        }
        for (unsigned char ch : exact) {
            const bool exactChar = (ch >= 'a' && ch <= 'z')
                || (ch >= 'A' && ch <= 'Z')
                || (ch >= '0' && ch <= '9')
                || ch == '.' || ch == '-' || ch == '_' || ch == '+';
            if (!exactChar) {
                return std::unexpected(TemplateSpecError{std::format(
                    "invalid template spec '{}': version '{}' must be an "
                    "exact version key, not a range or unsafe value",
                    spelling, exact)});
            }
        }
        version = std::string(exact);
    }

    auto package = mcpp::pm::parse_package_selector(packageSpelling);
    if (!package) {
        return std::unexpected(TemplateSpecError{std::format(
            "invalid template spec '{}': {}", spelling,
            package.error().message)});
    }

    return TemplateSpec{
        .package = std::move(*package),
        .version = std::move(version),
        .templateName = std::move(templateName),
        .legacyList = legacyList,
    };
}

struct TemplateMeta {
    std::string description;
    bool        isDefault = false;
    std::string postMessage;
    // [template.inject] self = { features = [...] }
    std::vector<std::string> injectSelfFeatures;
};

std::expected<TemplateMeta, std::string>
load_meta(const std::filesystem::path& templateDir) {
    auto metaPath = templateDir / "template.toml";
    if (!std::filesystem::exists(metaPath)) {
        return std::unexpected(std::format(
            "template '{}' has no template.toml", templateDir.filename().string()));
    }
    auto doc = mcpp::libs::toml::parse_file(metaPath);
    if (!doc) {
        return std::unexpected(std::format(
            "template '{}': bad template.toml: {}",
            templateDir.filename().string(), doc.error().message));
    }

    TemplateMeta meta;
    if (auto v = doc->get_string("template.description"))  meta.description = *v;
    if (auto v = doc->get_string("template.post_message")) meta.postMessage = *v;
    if (auto* t = doc->get_table("template")) {
        if (auto it = t->find("default"); it != t->end() && it->second.is_bool())
            meta.isDefault = it->second.as_bool();
        if (auto it = t->find("inject"); it != t->end() && it->second.is_table()) {
            auto& inj = it->second.as_table();
            if (auto self = inj.find("self");
                self != inj.end() && self->second.is_table()) {
                auto& st = self->second.as_table();
                if (auto f = st.find("features");
                    f != st.end() && f->second.is_array()) {
                    for (auto& fv : f->second.as_array())
                        if (fv.is_string())
                            meta.injectSelfFeatures.push_back(fv.as_string());
                }
            }
        }
    }
    return meta;
}

struct TemplateEntry {
    std::string  name;
    TemplateMeta meta;
};

std::string template_choice_list(
    const std::vector<TemplateEntry>& entries) {
    std::string choices;
    for (auto const& entry : entries) {
        if (!choices.empty()) choices += ", ";
        choices += entry.name;
    }
    return choices;
}

std::expected<const TemplateEntry*, std::string>
select_template(const std::vector<TemplateEntry>& entries,
                std::optional<std::string_view> requested) {
    if (entries.empty()) {
        return std::unexpected(
            "package provides no templates (templates/ is empty)");
    }

    std::size_t defaults = 0;
    const TemplateEntry* explicitDefault = nullptr;
    for (auto const& entry : entries) {
        if (!entry.meta.isDefault) continue;
        ++defaults;
        explicitDefault = &entry;
    }
    if (defaults > 1) {
        return std::unexpected(
            "package declares more than one default template (template.toml "
            "`default = true` must appear at most once)");
    }

    if (requested) {
        for (auto const& entry : entries)
            if (entry.name == *requested) return &entry;
        return std::unexpected(std::format(
            "no template '{}'; available templates: {}",
            *requested, template_choice_list(entries)));
    }
    if (explicitDefault) return explicitDefault;
    if (entries.size() == 1) return &entries.front();
    return std::unexpected(std::format(
        "package declares no default template; choose one explicitly: {}",
        template_choice_list(entries)));
}

// Enumerate templates/<name>/ entries of a package root (sorted by name).
std::expected<std::vector<TemplateEntry>, std::string>
list_templates(const std::filesystem::path& packageRoot) {
    auto dir = packageRoot / "templates";
    if (!std::filesystem::exists(dir)) {
        return std::unexpected("package ships no templates/ directory");
    }
    std::vector<TemplateEntry> out;
    std::error_code ec;
    std::filesystem::directory_iterator iterator(dir, ec), end;
    if (ec) {
        return std::unexpected(std::format(
            "cannot enumerate package templates: {}", ec.message()));
    }
    while (iterator != end) {
        const auto e = *iterator;
        auto status = e.symlink_status(ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot inspect template provider entry '{}': {}",
                e.path().string(), ec.message()));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(std::format(
                "template provider entry '{}' is a symlink", e.path().string()));
        }
        if (!std::filesystem::is_directory(status)) {
            iterator.increment(ec);
            if (ec) {
                return std::unexpected(std::format(
                    "cannot enumerate package templates: {}", ec.message()));
            }
            continue;
        }
        auto templateName = e.path().filename().string();
        auto parsedName = mcpp::pm::parse_package_selector(templateName);
        if (!parsedName || parsedName->namespace_) {
            return std::unexpected(std::format(
                "template provider contains invalid template directory '{}': "
                "name must be one ASCII atom", templateName));
        }
        auto meta = load_meta(e.path());
        if (!meta) return std::unexpected(meta.error());
        out.push_back({std::move(templateName), std::move(*meta)});
        iterator.increment(ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot enumerate package templates: {}", ec.message()));
        }
    }
    std::ranges::sort(out, {}, &TemplateEntry::name);
    int defaults = 0;
    for (auto& t : out) defaults += t.meta.isDefault ? 1 : 0;
    if (defaults > 1) {
        return std::unexpected(
            "package declares more than one default template (template.toml "
            "`default = true` must appear at most once)");
    }
    return out;
}

// The placeholder vocabulary is deliberately minimal and mcpp-owned. Values
// are appended by a single-pass renderer and are never scanned as template
// source again.
struct RenderVars {
    std::string projectName;
    std::string projectNamespace;
    std::string projectQualifiedName;
    std::string templatePackageNamespace;
    std::string templatePackageName;
    std::string templatePackageSelector;
    std::string templatePackageVersion;
    std::string templateName;
};

struct RenderError {
    std::string message;
};

std::expected<std::string, RenderError>
render_tokens(std::string_view input, const RenderVars& vars) {
    auto value_for = [&](std::string_view token)
        -> std::optional<std::string_view> {
        if (token == "project.name") return vars.projectName;
        if (token == "project.namespace") return vars.projectNamespace;
        if (token == "project.qualifiedName") return vars.projectQualifiedName;
        if (token == "template.package.namespace")
            return vars.templatePackageNamespace;
        if (token == "template.package.name") return vars.templatePackageName;
        if (token == "template.package.selector")
            return vars.templatePackageSelector;
        if (token == "template.package.version")
            return vars.templatePackageVersion;
        if (token == "template.name") return vars.templateName;
        // One compatibility train for existing template packages.
        if (token == "self.name") return vars.templatePackageSelector;
        if (token == "self.version") return vars.templatePackageVersion;
        return std::nullopt;
    };

    std::string out;
    out.reserve(input.size());
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        auto open = input.find("{{", cursor);
        if (open == std::string_view::npos) {
            out.append(input.substr(cursor));
            break;
        }
        out.append(input.substr(cursor, open - cursor));
        auto close = input.find("}}", open + 2);
        if (close == std::string_view::npos) {
            return std::unexpected(RenderError{std::format(
                "unterminated template token at byte {}", open)});
        }
        auto token = input.substr(open + 2, close - open - 2);
        auto value = value_for(token);
        if (!value) {
            return std::unexpected(RenderError{std::format(
                "unknown template token '{{{{{}}}}}'", token)});
        }
        out.append(*value); // intentionally never rescanned
        cursor = close + 2;
    }
    return out;
}

class ScaffoldTransaction {
    std::filesystem::path finalPath_;
    std::filesystem::path stagingPath_;
    bool committed_ = false;

    ScaffoldTransaction(std::filesystem::path finalPath,
                        std::filesystem::path stagingPath)
        : finalPath_(std::move(finalPath)),
          stagingPath_(std::move(stagingPath)) {}

public:
    ScaffoldTransaction(const ScaffoldTransaction&) = delete;
    ScaffoldTransaction& operator=(const ScaffoldTransaction&) = delete;
    ScaffoldTransaction(ScaffoldTransaction&& other) noexcept
        : finalPath_(std::move(other.finalPath_)),
          stagingPath_(std::move(other.stagingPath_)),
          committed_(other.committed_) {
        other.committed_ = true;
        other.stagingPath_.clear();
    }
    ScaffoldTransaction& operator=(ScaffoldTransaction&&) = delete;

    ~ScaffoldTransaction() {
        if (committed_ || stagingPath_.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(stagingPath_, ec);
    }

    static std::expected<ScaffoldTransaction, std::string>
    begin(const std::filesystem::path& parent, std::string_view name) {
        std::error_code ec;
        if (!std::filesystem::is_directory(parent, ec) || ec) {
            return std::unexpected(std::format(
                "scaffold parent '{}' is not a readable directory",
                parent.string()));
        }
        auto finalPath = parent / std::string(name);
        if (std::filesystem::exists(finalPath, ec) || ec) {
            return std::unexpected(std::format(
                "'{}' already exists", finalPath.string()));
        }

        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto nonce = (static_cast<std::uint64_t>(random()) << 32)
                ^ static_cast<std::uint64_t>(random())
                ^ static_cast<std::uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            auto stagingPath = parent / std::format(
                ".mcpp-new-{:016x}", nonce);
            ec.clear();
            if (std::filesystem::create_directory(stagingPath, ec)) {
                return ScaffoldTransaction{
                    std::move(finalPath), std::move(stagingPath)};
            }
            if (ec && ec != std::errc::file_exists) {
                return std::unexpected(std::format(
                    "cannot create scaffold staging directory in '{}': {}",
                    parent.string(), ec.message()));
            }
        }
        return std::unexpected(std::format(
            "cannot allocate a unique scaffold staging directory in '{}'",
            parent.string()));
    }

    const std::filesystem::path& staging_path() const { return stagingPath_; }
    const std::filesystem::path& final_path() const { return finalPath_; }

    std::expected<void, std::string> commit() {
        if (committed_) return {};
        if (auto synced = mcpp::platform::sync_directory(stagingPath_);
            !synced) {
            return synced;
        }
        auto renamed = mcpp::platform::atomic_rename_directory_no_replace(
            stagingPath_, finalPath_);
        if (!renamed) return renamed;
        committed_ = true;
        // Once the no-replace rename commits, the operation has succeeded.
        // Persist its parent entry where supported; an exotic filesystem that
        // refuses this post-commit sync must not turn a visible project into a
        // reported failure/partial-output contradiction.
        (void)mcpp::platform::sync_directory(finalPath_.parent_path());
        return {};
    }
};

std::expected<void, std::string>
write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        return std::unexpected(std::format(
            "cannot open '{}' for writing", path.string()));
    }
    os.write(content.data(), static_cast<std::streamsize>(content.size()));
    os.flush();
    if (!os) {
        return std::unexpected(std::format(
            "write '{}' failed", path.string()));
    }
    os.close();
    if (!os) {
        return std::unexpected(std::format(
            "close '{}' failed", path.string()));
    }
    return mcpp::platform::sync_regular_file(path);
}

// Instantiate templateDir into destDir: `.in` files are rendered (suffix
// stripped), everything else copied verbatim; template.toml is metadata
// only and never copied.
std::expected<void, std::string>
instantiate(const std::filesystem::path& templateDir,
            const std::filesystem::path& destDir,
            const RenderVars& vars) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(templateDir, ec);
    if (ec) {
        return std::unexpected(std::format(
            "cannot enumerate template '{}': {}",
            templateDir.string(), ec.message()));
    }
    std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto e = *iterator;
        auto status = e.symlink_status(ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot inspect template entry '{}': {}",
                e.path().string(), ec.message()));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(std::format(
                "template entry '{}' is a symlink; template payloads must be "
                "self-contained data", e.path().string()));
        }
        auto rel = std::filesystem::relative(e.path(), templateDir, ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot relativize template entry '{}': {}",
                e.path().string(), ec.message()));
        }
        if (rel == "template.toml") {
            iterator.increment(ec);
            if (ec) {
                return std::unexpected(std::format(
                    "enumerating template '{}' failed: {}",
                    templateDir.string(), ec.message()));
            }
            continue;
        }
        auto dest = destDir / rel;
        if (std::filesystem::is_directory(status)) {
            std::filesystem::create_directories(dest, ec);
            if (ec) {
                return std::unexpected(std::format(
                    "create directory '{}' failed: {}",
                    dest.string(), ec.message()));
            }
            iterator.increment(ec);
            if (ec) {
                return std::unexpected(std::format(
                    "enumerating template '{}' failed: {}",
                    templateDir.string(), ec.message()));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            return std::unexpected(std::format(
                "template entry '{}' is not a regular file", rel.string()));
        }
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format(
                "create directory '{}' failed: {}",
                dest.parent_path().string(), ec.message()));
        }
        if (rel.extension() == ".in") {
            std::ifstream is(e.path(), std::ios::binary);
            if (!is) {
                return std::unexpected(std::format(
                    "cannot read template input '{}'", rel.string()));
            }
            std::stringstream ss;
            ss << is.rdbuf();
            if (is.bad()) {
                return std::unexpected(std::format(
                    "read template input '{}' failed", rel.string()));
            }
            auto rendered = render_tokens(ss.str(), vars);
            if (!rendered) return std::unexpected(rendered.error().message);
            dest.replace_extension();      // strip ".in"
            if (auto written = write_text_file(dest, *rendered); !written)
                return written;
        } else {
            ec.clear();
            std::filesystem::copy_file(
                e.path(), dest,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return std::unexpected(std::format(
                    "copy '{}' failed: {}", rel.string(), ec.message()));
            }
            if (auto synced = mcpp::platform::sync_regular_file(dest); !synced)
                return synced;
        }
        iterator.increment(ec);
        if (ec) {
            return std::unexpected(std::format(
                "enumerating template '{}' failed: {}",
                templateDir.string(), ec.message()));
        }
    }
    return {};
}

// Ensure the generated manifest depends on the template's own package.
// If the template already declares it (typically via {{self.version}}),
// nothing is injected — template wins.
std::expected<void, std::string>
inject_self_dependency(const std::filesystem::path& manifestPath,
                       const RenderVars& vars,
                       const std::vector<std::string>& features) {
    std::ifstream is(manifestPath, std::ios::binary);
    if (!is) return std::unexpected(std::format(
        "cannot read '{}'", manifestPath.string()));
    std::stringstream ss;
    ss << is.rdbuf();
    if (is.bad()) return std::unexpected(std::format(
        "read '{}' failed", manifestPath.string()));
    std::string content = ss.str();
    is.close();

    auto parsed = mcpp::manifest::parse_string(content, manifestPath);
    if (!parsed) return std::unexpected(std::format(
        "generated manifest is invalid: {}", parsed.error().format()));
    for (auto const& [mapKey, dep] : parsed->dependencies) {
        const auto shortName = dep.shortName.empty() ? mapKey : dep.shortName;
        if (dep.namespace_ == vars.templatePackageNamespace
            && shortName == vars.templatePackageName) {
            return {}; // template already declared this exact PackageId
        }
    }

    auto selector = mcpp::pm::parse_package_selector(
        vars.templatePackageSelector);
    if (!selector) return std::unexpected(selector.error().message);
    auto coordinate = mcpp::pm::normalize_package_selector(*selector);
    if (coordinate.namespace_ != vars.templatePackageNamespace
        || coordinate.shortName != vars.templatePackageName) {
        return std::unexpected(
            "template self-dependency selector disagrees with resolved PackageId");
    }

    auto edited = mcpp::manifest::upsert_dependency_text(content, {
        .namespace_ = vars.templatePackageNamespace,
        .shortName = vars.templatePackageName,
        .version = vars.templatePackageVersion,
        .features = features,
    });
    if (!edited) return std::unexpected(edited.error());
    return write_text_file(manifestPath, *edited);
}

} // namespace mcpp::scaffold
