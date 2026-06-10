// mcpp.fetcher.progress — xlings NDJSON install events -> ui renderer
// adapters, plus the path-shortening context used in status output.
// Bodies moved verbatim from the CLI layer. Zero behavior change
// (InstallProgressHandler was renamed InstallProgressHandler for its new home).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.fetcher.progress;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.log;
import mcpp.ui;

namespace mcpp::fetcher {


// ─── Install-time progress display ───────────────────────────────────
//
// xlings emits NDJSON events on stdout via `xlings interface install_packages
// --args ...` (see fetcher.cppm). The events we care about for UX are:
//
//   {"kind":"data","dataKind":"download_progress","payload":{
//     "elapsedSec": 2.0,
//     "files": [{"name":"...", "downloadedBytes":..., "totalBytes":..., "finished":bool, ...}],
//     ...
//   }}
//
// We parse the first file in the `files` array (xlings serializes the
// currently-active download first) and feed (current, total) to a
// ui::ProgressBar so the user sees a "Downloading <pkg> [====   ]
// 45 MB / 110 MB" line.

struct InstallProgressFile {
    std::string name;
    double      downloaded = 0;
    double      total      = 0;
    bool        started    = false;
    bool        finished   = false;
};

namespace {

// Extract one `{ ... }` object starting at payload[*pos], moving *pos past
// the closing `}`. Returns the slice or empty when no object is here.
std::string_view scan_one_object(std::string_view payload, std::size_t* pos) {
    auto p = *pos;
    while (p < payload.size() && (payload[p] == ' ' || payload[p] == '\n')) ++p;
    if (p >= payload.size() || payload[p] != '{') { *pos = p; return {}; }
    auto start = p;
    int depth = 0;
    bool in_string = false;
    for (; p < payload.size(); ++p) {
        char c = payload[p];
        if (in_string) {
            if (c == '\\' && p + 1 < payload.size()) { ++p; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"')      in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) { ++p; break; } }
    }
    *pos = p;
    return payload.substr(start, (p == payload.size() ? p : p) - start);
}

InstallProgressFile parse_one_install_file(std::string_view obj) {
    auto get_str = [&](std::string_view key) -> std::string {
        std::string n = std::format("\"{}\":\"", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return "";
        q += n.size();
        std::string out;
        while (q < obj.size() && obj[q] != '"') {
            if (obj[q] == '\\' && q + 1 < obj.size()) { out.push_back(obj[q+1]); q += 2; continue; }
            out.push_back(obj[q++]);
        }
        return out;
    };
    auto get_num = [&](std::string_view key) -> double {
        std::string n = std::format("\"{}\":", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return 0;
        q += n.size();
        auto e = q;
        while (e < obj.size()
            && (std::isdigit(static_cast<unsigned char>(obj[e]))
                || obj[e] == '.' || obj[e] == '-' || obj[e] == '+'
                || obj[e] == 'e' || obj[e] == 'E')) ++e;
        try { return std::stod(std::string(obj.substr(q, e - q))); }
        catch (...) { return 0; }
    };
    auto get_bool = [&](std::string_view key) -> bool {
        std::string n = std::format("\"{}\":", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return false;
        q += n.size();
        return obj.size() - q >= 4 && obj.substr(q, 4) == "true";
    };

    InstallProgressFile f;
    f.name       = get_str("name");
    f.downloaded = get_num("downloadedBytes");
    f.total      = get_num("totalBytes");
    f.started    = get_bool("started");
    f.finished   = get_bool("finished");
    return f;
}

} // namespace

// Parse every entry in the payload's `files` array. xlings emits an
// array-of-files for download_progress events even when only one is
// active, and during multi-package installs (gcc → glibc / binutils /
// linux-headers / gcc-runtime / gcc) the order of entries shifts as
// each file starts and finishes. Reading just the first one would
// flicker between names and re-emit the static "Downloading <pkg>"
// line every time the first slot rotates.
std::vector<InstallProgressFile>
parse_all_install_files(std::string_view payload)
{
    std::vector<InstallProgressFile> out;
    constexpr std::string_view kKey{"\"files\":["};
    auto p = payload.find(kKey);
    if (p == std::string_view::npos) return out;
    p += kKey.size();
    while (p < payload.size()) {
        while (p < payload.size() && (payload[p] == ' ' || payload[p] == '\n'
                                      || payload[p] == ',')) ++p;
        if (p >= payload.size() || payload[p] == ']') break;
        if (payload[p] != '{') break;
        auto obj = scan_one_object(payload, &p);
        if (obj.empty()) break;
        auto f = parse_one_install_file(obj);
        if (!f.name.empty()) out.push_back(std::move(f));
    }
    return out;
}

// Pull a top-level numeric field out of a payload JSON string. Cheap;
// only used for `elapsedSec` which we trust to be a plain number.
double extract_payload_number(std::string_view payload, std::string_view key) {
    std::string n = std::format("\"{}\":", key);
    auto q = payload.find(n);
    if (q == std::string_view::npos) return 0;
    q += n.size();
    auto e = q;
    while (e < payload.size()
        && (std::isdigit(static_cast<unsigned char>(payload[e]))
            || payload[e] == '.' || payload[e] == '-' || payload[e] == '+'
            || payload[e] == 'e' || payload[e] == 'E')) ++e;
    try { return std::stod(std::string(payload.substr(q, e - q))); }
    catch (...) { return 0; }
}

// Build the PathContext used to shorten user-visible paths in status
// output. project_root may be empty (for verbs that don't need it).
export mcpp::ui::PathContext make_path_ctx(const mcpp::config::GlobalConfig* cfg,
                                    std::filesystem::path project_root = {})
{
    mcpp::ui::PathContext ctx;
    ctx.project_root = std::move(project_root);
    if (cfg) ctx.mcpp_home = cfg->mcppHome;
    if (auto* h = std::getenv("HOME"); h && *h) ctx.home = h;
    return ctx;
}

// Map a decoded NDJSON `download_progress` files[] snapshot onto the neutral
// `mcpp::ui::DownloadFile` the centralized renderer consumes.
template <class File>
std::vector<mcpp::ui::DownloadFile> to_ui_download_files(const std::vector<File>& files) {
    std::vector<mcpp::ui::DownloadFile> out;
    out.reserve(files.size());
    for (auto& f : files) {
        if constexpr (requires { f.downloadedBytes; }) {
            out.push_back({ f.name,
                            static_cast<std::size_t>(f.downloadedBytes),
                            static_cast<std::size_t>(f.totalBytes),
                            f.started, f.finished });
        } else {
            out.push_back({ f.name,
                            static_cast<std::size_t>(f.downloaded),
                            static_cast<std::size_t>(f.total),
                            f.started, f.finished });
        }
    }
    return out;
}

// Adapter from `mcpp::config::BootstrapProgress` (xlings download_progress
// event) to the centralized download renderer. Used by load_or_init() during
// the one-time sandbox bootstrap (xim:patchelf, xim:ninja + transitive deps).
export mcpp::config::BootstrapProgressCallback make_bootstrap_progress_callback() {
    auto progress = std::make_shared<mcpp::ui::DownloadProgress>();
    return [progress](const mcpp::config::BootstrapProgress& ev) {
        auto files = to_ui_download_files(ev.files);
        progress->update(files, ev.elapsedSec);
    };
}

// EventHandler that forwards xlings `download_progress` events to the same
// centralized renderer. Used for toolchain, builtin-index and custom-index
// installs alike, so all three show identical UI.
export struct InstallProgressHandler : mcpp::fetcher::EventHandler {
    mcpp::ui::DownloadProgress progress_;

    void on_data(const mcpp::fetcher::DataEvent& d) override {
        if (d.dataKind != "download_progress") return;
        auto files = parse_all_install_files(d.payloadJson);
        if (files.empty()) return;
        double elapsed = extract_payload_number(d.payloadJson, "elapsedSec");
        auto ui_files = to_ui_download_files(files);
        progress_.update(ui_files, elapsed);
    }

    void on_log(const mcpp::fetcher::LogEvent& e) override {
        if (e.level == "error")
            mcpp::log::error("xlings", e.message);
        else if (e.level == "warn")
            mcpp::log::warn("xlings", e.message);
        else
            mcpp::log::info("xlings", e.message);
        mcpp::log::verbose("xlings", std::format("[{}] {}", e.level, e.message));
    }

    void on_error(const mcpp::fetcher::ErrorEvent& e) override {
        mcpp::log::error("xlings", std::format("{}: {}", e.code, e.message));
        if (!e.hint.empty())
            mcpp::log::info("xlings", std::format("hint: {}", e.hint));
    }

    // progress_'s own destructor finishes the active bar.
    ~InstallProgressHandler() override = default;
};


} // namespace mcpp::fetcher
