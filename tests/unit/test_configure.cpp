#include <gtest/gtest.h>

import std;
import mcpp.source_kind;
import mcpp.build.configure;
import mcpp.build.plan;
import mcpp.toolchain.model;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-configure-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

TEST(ConfigurePrerequisites, StagesOnlyBmisNeededByLanguageTools) {
    TempDir temp;
    mcpp::build::BuildPlan plan;
    plan.outputDir = temp.path / "out";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.stdBmiPath = temp.path / "cache/std.pcm";
    plan.stdCompatBmiPath = temp.path / "cache/std.compat.pcm";
    write_file(plan.stdBmiPath, "std-bmi");
    write_file(plan.stdCompatBmiPath, "std-compat-bmi");

    mcpp::build::CompileUnit dep;
    dep.servedFromCache = true;
    dep.providesModule = "demo.dep";
    dep.cachedBmi = temp.path / "cache/demo.dep.pcm";
    dep.cachedObject = temp.path / "cache/demo.dep.o";
    dep.object = "obj/demo.dep.o";
    write_file(dep.cachedBmi, "dep-bmi");
    write_file(dep.cachedObject, "dep-object");
    plan.compileUnits.push_back(dep);

    auto staged = mcpp::build::stage_configure_prerequisites(plan);
    ASSERT_TRUE(staged.has_value()) << staged.error();
    EXPECT_TRUE(std::filesystem::exists(plan.outputDir / "pcm.cache/std.pcm"));
    EXPECT_TRUE(std::filesystem::exists(plan.outputDir / "pcm.cache/std.compat.pcm"));
    EXPECT_TRUE(std::filesystem::exists(plan.outputDir / "pcm.cache/demo.dep.pcm"));
    EXPECT_FALSE(std::filesystem::exists(plan.outputDir / dep.object));
}

TEST(ConfigurePrerequisites, MissingCachedBmiFailsBeforePublication) {
    TempDir temp;
    mcpp::build::BuildPlan plan;
    plan.outputDir = temp.path / "out";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;

    mcpp::build::CompileUnit dep;
    dep.servedFromCache = true;
    dep.providesModule = "demo.dep";
    dep.cachedBmi = temp.path / "missing/demo.dep.pcm";
    plan.compileUnits.push_back(dep);

    auto staged = mcpp::build::stage_configure_prerequisites(plan);
    ASSERT_FALSE(staged.has_value());
    EXPECT_NE(staged.error().find("demo.dep"), std::string::npos);
}

} // namespace
