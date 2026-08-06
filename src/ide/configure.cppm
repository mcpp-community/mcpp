// mcpp.ide.configure — resolve a build plan and publish its CDB before Ninja.
// 该模块只负责 IDE 配置阶段，不驱动普通源文件编译或最终链接。
module;
#include <cstdio>
export module mcpp.ide.configure;
import std;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.prepare;
import mcpp.ide.model;
import mcpp.libs.json;
import mcpp.project;
import mcpp.toolchain.fingerprint;
import mcpp.ui;

namespace mcpp::ide {
namespace {
using Json = nlohmann::ordered_json;

// configure 需要暂时屏蔽 prepare_build 的人类状态输出；用 RAII 保证
// 解析失败或异常返回时也恢复调用者的 quiet 状态。
struct QuietGuard {
    const bool previous = mcpp::ui::is_quiet();

    QuietGuard() { mcpp::ui::set_quiet(true); }
    ~QuietGuard() { mcpp::ui::set_quiet(previous); }
};

std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result.push_back(',');
        result += value;
    }
    return result;
}

std::string toolchain_fingerprint(const mcpp::toolchain::Fingerprint& fingerprint) {
    // Build fingerprint 还包含 flags、依赖和 BMI 状态；这些变化应只使
    // snapshotId 变化，不能让同一个工具链配置在 IDE 中产生新配置项。
    std::string identity;
    for (std::size_t i = 0; i < 5; ++i) {
        if (i != 0) identity.push_back('\x1f');
        identity += fingerprint.parts[i];
    }
    return "toolchain-fnv1a64:" + mcpp::toolchain::hash_string(identity);
}

ConfigurationSelectors configuration_selectors(const Selectors& selectors) {
    return {
        .package = selectors.package, .workspace = selectors.workspace,
        .profile = selectors.profile.value_or(""), .target = selectors.target.value_or(""),
        .features = selectors.features, .capabilities = selectors.capabilities,
        .includeDevDependencies = selectors.includeDevDependencies,
    };
}

Json event(std::uint64_t seq, std::string_view type) {
    Json value = Json::object();
    value["schemaVersion"] = 1; value["seq"] = seq; value["type"] = type;
    return value;
}
} // namespace

export struct ConfigureRequest {
    std::filesystem::path start;
    Selectors selectors;
};

export struct ConfigureResult {
    std::string projectId;
    std::string configurationId;
    std::string snapshotId;
    std::filesystem::path projectRoot;
    std::filesystem::path compileCommands;
    std::filesystem::path compatibilityCompileCommands;
    std::string toolchain;
    std::string toolchainFingerprint;
    std::size_t compileCommandCount = 0;
    std::filesystem::path stdModule;
    std::string stdModuleState;
};

export std::expected<ConfigureResult, std::string>
configure_project(const ConfigureRequest& request) {
    auto start = request.start.empty() ? std::filesystem::current_path() : request.start;
    auto root = mcpp::project::find_manifest_root(start);
    if (!root) return std::unexpected("no mcpp.toml found in current directory or any parent");
    if (request.selectors.workspace) {
        return std::unexpected(
            "workspace configure fan-out is not available yet; select one member with --package");
    }

    mcpp::build::BuildOverrides overrides;
    overrides.project_root = *root;
    if (request.selectors.package) overrides.package_filter = *request.selectors.package;
    if (request.selectors.profile) overrides.profile = *request.selectors.profile;
    if (request.selectors.target) overrides.target_triple = *request.selectors.target;
    overrides.features = join(request.selectors.features);
    overrides.capabilities = join(request.selectors.capabilities);
    overrides.materialize_std_modules = false;

    // configure 的结构化 stdout 不能混入普通进度；mcpp 自身写入的项目文件
    // 仍保持原有 prepare_build 语义，但状态行转为诊断/事件。
    QuietGuard quiet;
    auto context = mcpp::build::prepare_build(
        /*print_fingerprint=*/false, request.selectors.includeDevDependencies, {},
        std::move(overrides));
    if (!context) return std::unexpected(context.error());

    const auto configSelectors = configuration_selectors(request.selectors);
    auto resolvedSelectors = configSelectors;
    resolvedSelectors.profile = context->profile;
    resolvedSelectors.target = context->tc.targetTriple;
    resolvedSelectors.cacheMode = std::string(
        mcpp::build::cache_mode_name(context->cacheMode));
    resolvedSelectors.cppStandard = context->manifest.package.standard;
    const auto toolchainFingerprint = toolchain_fingerprint(context->fp);
    const auto configId = configuration_id(
        context->projectRoot, resolvedSelectors, toolchainFingerprint);
    const auto projectId = std::format("project-fnv1a64:{}",
        mcpp::toolchain::hash_string(context->projectRoot.generic_string()));
    auto flags = mcpp::build::compute_flags(context->plan);
    const auto content = mcpp::build::emit_compile_commands(context->plan, flags);
    auto parsed = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array())
        return std::unexpected("mcpp produced an invalid compile database");

    const auto ideRoot = context->projectRoot / ".mcpp" / "ide" / "replies";
    const auto snapshotCdb = ideRoot / std::format("compile_commands-{}.json", configId);
    if (auto written = mcpp::build::write_fresh_compile_commands(snapshotCdb, content); !written)
        return std::unexpected(written.error());
    const auto compatibilityCdb = context->projectRoot / "compile_commands.json";
    if (auto written = mcpp::build::write_fresh_compile_commands(compatibilityCdb, content); !written)
        return std::unexpected(written.error());

    const auto snapshotId = std::format("snapshot-fnv1a64:{}",
        mcpp::toolchain::hash_string(configId + "\x1f" + content));
    return ConfigureResult{
        .projectId = projectId, .configurationId = configId, .snapshotId = snapshotId,
        .projectRoot = context->projectRoot, .compileCommands = snapshotCdb,
        .compatibilityCompileCommands = compatibilityCdb, .toolchain = context->tc.label(),
        .toolchainFingerprint = toolchainFingerprint, .compileCommandCount = parsed.size(),
        .stdModule = context->plan.stdBmiPath,
        .stdModuleState = context->plan.stdBmiPath.empty()
            ? "not-required"
            : (std::filesystem::is_regular_file(context->plan.stdBmiPath) ? "ready" : "pending"),
    };
}

export std::vector<std::string> configure_events(const ConfigureResult& result) {
    std::vector<std::string> lines;
    auto started = event(1, "operation-started");
    started["operation"] = "configure"; started["configurationId"] = result.configurationId;
    lines.push_back(started.dump());
    auto published = event(2, "snapshot-published");
    published["phase"] = "configured"; published["state"] = "configured";
    published["projectId"] = result.projectId; published["configurationId"] = result.configurationId;
    published["snapshotId"] = result.snapshotId;
    published["compileCommands"] = result.compileCommands.generic_string();
    published["compatibilityCompileCommands"] = result.compatibilityCompileCommands.generic_string();
    published["compileCommandCount"] = result.compileCommandCount;
    published["toolchain"] = result.toolchain;
    published["toolchainFingerprint"] = result.toolchainFingerprint;
    if (!result.stdModule.empty()) {
        auto stdModule = Json::object();
        stdModule["kind"] = "std-module";
        stdModule["path"] = result.stdModule.generic_string();
        stdModule["state"] = result.stdModuleState;
        published["stdModule"] = std::move(stdModule);
    }
    lines.push_back(published.dump());
    auto finished = event(3, "operation-finished");
    finished["operation"] = "configure"; finished["status"] = "success";
    finished["phase"] = "configured"; finished["configurationId"] = result.configurationId;
    lines.push_back(finished.dump());
    return lines;
}

export std::string configure_error_event(std::string_view message) {
    auto value = event(1, "diagnostic");
    value["diagnostic"] = Json::object({
        {"code", "MCPP_IDE_CONFIGURE_FAILED"}, {"severity", "error"},
        {"source", "mcpp"}, {"message", message},
    });
    return value.dump();
}

} // namespace mcpp::ide
