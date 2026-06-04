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

export namespace mcpp::scaffold {

// `--template` package-form SPEC:
//   pkg | pkg:tmpl | pkg@ver | pkg@ver:tmpl | pkg:   (trailing ':' = list)
struct TemplateSpec {
    std::string pkg;
    std::string version;        // "" = latest
    std::string tmpl;           // "" = the package's default template
    bool        listOnly = false;
};

TemplateSpec parse_spec(std::string_view s) {
    TemplateSpec spec;
    std::string_view left = s;
    if (auto c = s.find(':'); c != std::string_view::npos) {
        left = s.substr(0, c);
        auto right = s.substr(c + 1);
        if (right.empty()) spec.listOnly = true;
        else               spec.tmpl = std::string(right);
    }
    if (auto a = left.find('@'); a != std::string_view::npos) {
        spec.pkg     = std::string(left.substr(0, a));
        spec.version = std::string(left.substr(a + 1));
    } else {
        spec.pkg = std::string(left);
    }
    return spec;
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
        auto meta = load_meta(e.path());
        if (!meta) return std::unexpected(meta.error());
        out.push_back({e.path().filename().string(), std::move(*meta)});
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
