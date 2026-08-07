// mcpp.ide.configure — resolve a build plan and publish its CDB before Ninja.
// 该模块只负责 IDE 配置阶段，不驱动普通源文件编译或最终链接。
module;
#include <cstdio>
export module mcpp.ide.configure;
import std;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.plan;
import mcpp.build.prepare;
import mcpp.build.stage;
import mcpp.build.test_targets;
import mcpp.ide.model;
import mcpp.ide.publish;
import mcpp.libs.json;
import mcpp.project;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.model;
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

Json event(std::uint64_t seq, std::string_view type, std::string_view operationId) {
    Json value = Json::object();
    value["schemaVersion"] = 1; value["seq"] = seq; value["type"] = type;
    value["operationId"] = operationId;
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
run_configure_safely(const std::function<
    std::expected<ConfigureResult, std::string>()>& operation) {
    try {
        return operation();
    } catch (const std::exception& error) {
        return std::unexpected(std::format(
            "IDE configure raised an exception: {}", error.what()));
    } catch (...) {
        return std::unexpected("IDE configure raised an unknown exception");
    }
}

// configured snapshot 的身份同时绑定配置、当前 resolved build fingerprint
// 和 CDB 内容；精简 toolchain fingerprint 只用于 configurationId，不能替代
// 这里的 BuildContext::fp。
export std::string configured_snapshot_id(std::string_view configurationId,
                                          std::string_view buildFingerprint,
                                          std::string_view cdbHash) {
    return std::format("snapshot-fnv1a64:{}",
        mcpp::toolchain::hash_string(std::string(configurationId) + "\x1f"
                                     + std::string(buildFingerprint) + "\x1f"
                                     + std::string(cdbHash)));
}

export std::string new_operation_id() {
    static std::atomic<std::uint64_t> sequence = 0;
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
    const auto monotonic = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto entropy = std::random_device{}();
    return std::format("operation-fnv1a64:{}", mcpp::toolchain::hash_string(
        std::format("{}:{}:{}:{}", wall, monotonic, entropy, serial)));
}

export std::expected<void, std::string>
stage_cached_module_prerequisites(const mcpp::build::BuildPlan& plan) {
    const auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    const auto bmiDir = plan.outputDir / traits.bmiDir;
    // BMI 来源由工具链 fingerprint 和缓存 key 约束；与 Ninja 的
    // --verify size 一致可避免 clangd 映射目标文件时再次打开它比较内容。
    const mcpp::build::stage::StageOptions stageOptions{
        .verify = mcpp::build::stage::Verify::Size};

    auto stage = [&](const std::filesystem::path& source)
        -> std::expected<void, std::string> {
        if (source.empty()) return {};
        // Keep this destination spelling identical to ninja_backend's BMI
        // output: the CDB is published only after every referenced prerequisite
        // is present at the path clangd will open.
        auto staged = mcpp::build::stage::stage_file(
            source, bmiDir / source.filename(), stageOptions);
        if (!staged) return std::unexpected(staged.error().message);
        return {};
    };

    // CDB 引用构建目录中的标准库 BMI；configure 不运行 Ninja，因此
    // 必须在发布前主动物化这两个前置产物。
    if (auto result = stage(plan.stdBmiPath); !result) return result;
    if (auto result = stage(plan.stdCompatBmiPath); !result) return result;

    // 全局依赖缓存命中时只 stage BMI，不复制对象文件。clangd 只需要
    // 模块语义产物，普通编译和链接仍由后续 build/test 负责。
    for (const auto& unit : plan.compileUnits) {
        if (!unit.servedFromCache || !unit.providesModule || unit.cachedBmi.empty())
            continue;
        if (auto result = stage(unit.cachedBmi); !result) return result;
    }
    return {};
}

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

    // configure 的结构化 stdout 不能混入普通进度；mcpp 自身写入的项目文件
    // 仍保持原有 prepare_build 语义，但状态行转为诊断/事件。
    QuietGuard quiet;
    auto tests = mcpp::build::discover_test_targets(
        *root, request.selectors.package.value_or(""));
    if (!tests) return std::unexpected(tests.error());
    // 测试 target 的真实构建语义包含 dev-dependencies；只把测试源放进 CDB
    // 却遗漏其依赖 include/define，clangd 仍会把测试文件解析成错误配置。
    const bool includeDevDependencies = request.selectors.includeDevDependencies
                                     || !tests->targets.empty();
    auto context = mcpp::build::prepare_build(
        /*print_fingerprint=*/false, includeDevDependencies,
        std::move(tests->targets),
        std::move(overrides));
    if (!context) return std::unexpected(context.error());
    // A failed stage must leave the previous CDB untouched and must not emit a
    // snapshot that clangd cannot load.
    if (auto staged = stage_cached_module_prerequisites(context->plan); !staged)
        return std::unexpected(staged.error());

    const auto configSelectors = configuration_selectors(request.selectors);
    auto resolvedSelectors = configSelectors;
    resolvedSelectors.profile = context->profile;
    resolvedSelectors.target = context->tc.targetTriple;
    resolvedSelectors.cacheMode = std::string(
        mcpp::build::cache_mode_name(context->cacheMode));
    resolvedSelectors.cppStandard = context->manifest.package.standard;
    resolvedSelectors.includeDevDependencies = includeDevDependencies;
    auto workspaceRoot = mcpp::project::find_workspace_root(context->projectRoot);
    if (workspaceRoot.empty()) workspaceRoot = context->projectRoot;
    workspaceRoot = physical_project_root(workspaceRoot);
    resolvedSelectors.package = canonical_member_selector(
        workspaceRoot, context->projectRoot);
    const auto toolchainFingerprint = toolchain_fingerprint(context->fp);
    const auto configId = configuration_id(
        workspaceRoot, resolvedSelectors, toolchainFingerprint);
    const auto projectId = project_id(workspaceRoot);
    auto flags = mcpp::build::compute_flags(context->plan);
    const auto content = mcpp::build::emit_compile_commands(context->plan, flags);
    auto parsed = nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array())
        return std::unexpected("mcpp produced an invalid compile database");

    const auto ideRoot = context->projectRoot / ".mcpp" / "ide" / "replies";
    // Reply 文件按 CDB 内容寻址：配置保持不变而 flags/source 变化时，旧
    // reply 仍可被 last-known-good 客户端读取；同时 hash 只含十六进制字符，
    // 不会把 configId 中的 ':' 带入 Windows 文件名。
    const auto cdbHash = mcpp::toolchain::hash_string(content);
    const auto snapshotCdb = ideRoot / std::format("compile_commands-{}.json", cdbHash);
    if (auto written = mcpp::build::write_fresh_compile_commands(snapshotCdb, content); !written)
        return std::unexpected(written.error());
    const auto compatibilityCdb = context->projectRoot / "compile_commands.json";

    const auto snapshotId = configured_snapshot_id(configId, context->fp.hex, cdbHash);
    const auto traits = mcpp::toolchain::bmi_traits(context->plan.toolchain);
    const auto stagedStdModule = context->plan.stdBmiPath.empty()
        ? std::filesystem::path{}
        : context->plan.outputDir / traits.bmiDir / context->plan.stdBmiPath.filename();
    ConfigureResult result{
        .projectId = projectId, .configurationId = configId, .snapshotId = snapshotId,
        .projectRoot = context->projectRoot, .compileCommands = snapshotCdb,
        .compatibilityCompileCommands = compatibilityCdb, .toolchain = context->tc.label(),
        .toolchainFingerprint = toolchainFingerprint, .compileCommandCount = parsed.size(),
        .stdModule = stagedStdModule,
        .stdModuleState = stagedStdModule.empty()
            ? "not-required"
            : (std::filesystem::is_regular_file(stagedStdModule) ? "ready" : "pending"),
    };
    auto published = publish_configured_snapshot(context->projectRoot, {
        .projectId = result.projectId,
        .configurationId = result.configurationId,
        .snapshotId = result.snapshotId,
        .phase = "configured",
        .projectRoot = result.projectRoot,
        .compileCommands = result.compileCommands,
        .compatibilityCompileCommands = result.compatibilityCompileCommands,
        .toolchain = result.toolchain,
        .toolchainFingerprint = result.toolchainFingerprint,
        .compileCommandCount = result.compileCommandCount,
    }, content);
    if (!published) return std::unexpected(published.error());
    return result;
}

export std::string configure_started_event(
    std::string_view operationId, std::string_view configurationId = {}) {
    auto started = event(1, "operation-started", operationId);
    started["operation"] = "configure";
    if (!configurationId.empty()) started["configurationId"] = configurationId;
    return started.dump();
}

export std::vector<std::string> configure_events(
    const ConfigureResult& result,
    std::string_view operationId,
    std::uint64_t firstSeq = 1,
    bool includeStarted = true) {
    std::vector<std::string> lines;
    auto seq = firstSeq;
    if (includeStarted) {
        auto started = event(seq++, "operation-started", operationId);
        started["operation"] = "configure";
        started["configurationId"] = result.configurationId;
        lines.push_back(started.dump());
    }
    auto published = event(seq++, "snapshot-published", operationId);
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
    auto finished = event(seq, "operation-finished", operationId);
    finished["operation"] = "configure"; finished["status"] = "success";
    finished["phase"] = "configured"; finished["configurationId"] = result.configurationId;
    lines.push_back(finished.dump());
    return lines;
}

export std::vector<std::string> configure_failure_events(
    std::string_view message,
    std::string_view operationId,
    std::uint64_t firstSeq = 1) {
    std::vector<std::string> lines;
    auto diagnostic = event(firstSeq, "diagnostic", operationId);
    diagnostic["diagnostic"] = Json::object({
        {"code", "MCPP_IDE_CONFIGURE_FAILED"}, {"severity", "error"},
        {"source", "mcpp"}, {"message", message},
    });
    lines.push_back(diagnostic.dump());

    auto finished = event(firstSeq + 1, "operation-finished", operationId);
    finished["operation"] = "configure";
    finished["status"] = "failed";
    finished["diagnosticCodes"] = Json::array({"MCPP_IDE_CONFIGURE_FAILED"});
    lines.push_back(finished.dump());
    return lines;
}

} // namespace mcpp::ide
