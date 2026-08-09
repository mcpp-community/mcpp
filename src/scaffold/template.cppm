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
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_directory()) continue;
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
    }
    if (ec) {
        return std::unexpected(std::format(
            "cannot enumerate package templates: {}", ec.message()));
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

// The placeholder vocabulary is deliberately minimal and mcpp-owned;
// template variability comes from packages shipping multiple templates,
// not from growing the renderer into a programming language.
struct RenderVars {
    std::string projectName;
    std::string selfName;
    std::string selfVersion;
};

std::string render_text(std::string text, const RenderVars& vars) {
    auto replace_all = [&](std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("{{project.name}}", vars.projectName);
    replace_all("{{self.name}}",    vars.selfName);
    replace_all("{{self.version}}", vars.selfVersion);
    return text;
}

// Instantiate templateDir into destDir: `.in` files are rendered (suffix
// stripped), everything else copied verbatim; template.toml is metadata
// only and never copied.
std::optional<std::string>
instantiate(const std::filesystem::path& templateDir,
            const std::filesystem::path& destDir,
            const RenderVars& vars) {
    std::error_code ec;
    for (auto& e : std::filesystem::recursive_directory_iterator(templateDir, ec)) {
        auto rel = std::filesystem::relative(e.path(), templateDir, ec);
        if (rel == "template.toml") continue;
        auto dest = destDir / rel;
        if (e.is_directory()) {
            std::filesystem::create_directories(dest, ec);
            continue;
        }
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (rel.extension() == ".in") {
            std::ifstream is(e.path());
            std::stringstream ss; ss << is.rdbuf();
            auto rendered = render_text(ss.str(), vars);
            dest.replace_extension();      // strip ".in"
            std::ofstream os(dest);
            os << rendered;
        } else {
            std::filesystem::copy_file(
                e.path(), dest,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return std::format("copy '{}' failed: {}",
                                   rel.string(), ec.message());
            }
        }
    }
    return std::nullopt;
}

// Ensure the generated manifest depends on the template's own package.
// If the template already declares it (typically via {{self.version}}),
// nothing is injected — template wins.
std::optional<std::string>
inject_self_dependency(const std::filesystem::path& manifestPath,
                       const RenderVars& vars,
                       const std::vector<std::string>& features) {
    std::ifstream is(manifestPath);
    if (!is) return std::format("cannot read '{}'", manifestPath.string());
    std::stringstream ss; ss << is.rdbuf();
    std::string content = ss.str();
    is.close();

    if (content.find(vars.selfName + " =") != std::string::npos
        || content.find(vars.selfName + "=") != std::string::npos) {
        return std::nullopt;   // already declared by the template
    }

    std::string depLine;
    if (features.empty()) {
        depLine = std::format("{} = \"{}\"\n", vars.selfName, vars.selfVersion);
    } else {
        std::string flist;
        for (auto& f : features) {
            if (!flist.empty()) flist += ", ";
            flist += std::format("\"{}\"", f);
        }
        depLine = std::format("{} = {{ version = \"{}\", features = [{}] }}\n",
                              vars.selfName, vars.selfVersion, flist);
    }

    constexpr std::string_view header = "[dependencies]";
    if (auto pos = content.find(header); pos != std::string::npos) {
        auto eol = content.find('\n', pos);
        if (eol == std::string::npos) content += "\n" + depLine;
        else content.insert(eol + 1, depLine);
    } else {
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += "\n[dependencies]\n" + depLine;
    }

    std::ofstream os(manifestPath);
    os << content;
    return std::nullopt;
}

} // namespace mcpp::scaffold
