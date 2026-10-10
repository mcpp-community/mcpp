#include <gtest/gtest.h>

import std;
import mcpp.cli.completion;
import mcpplibs.cmdline;

namespace {
namespace completion = mcpp::cli::completion;
namespace cl = mcpplibs::cmdline;

completion::App application() {
    return completion::App("mcpp")
        .option(cl::Option("offline").global())
        .subcommand(completion::App("build")
            .option(cl::Option("cache").takes_value())
            .option(cl::Option("package").short_name('p').takes_value())
            .option(cl::Option("output").short_name('o').takes_value().value_name("FILE")))
        .subcommand(completion::App("self")
            .subcommand(completion::App("config")
                .option(cl::Option("mirror").takes_value())))
        .subcommand(completion::App("secret").description("(internal) Hidden"));
}

completion::Result complete(std::initializer_list<std::string> words) {
    std::vector<std::string> tokens(words);
    return completion::candidates(application().command, tokens);
}
}

TEST(ShellCompletion, RootAndNestedCommandsComeFromTheParserDefinition) {
    EXPECT_EQ(complete({""}).words, (std::vector<std::string>{"build", "self"}));
    EXPECT_EQ(complete({"self", "c"}).words, (std::vector<std::string>{"config"}));
    EXPECT_EQ(complete({"--offline", "self", ""}).words, (std::vector<std::string>{"config"}));
}

TEST(ShellCompletion, LocalShortAndInheritedGlobalOptionsAreOffered) {
    EXPECT_EQ(complete({"build", "--o"}).words,
              (std::vector<std::string>{"--offline", "--output"}));
    EXPECT_EQ(complete({"build", "-o"}).words, (std::vector<std::string>{"-o"}));
    EXPECT_EQ(complete({"self", "config", "--o"}).words,
              (std::vector<std::string>{"--offline"}));
}

TEST(ShellCompletion, ValuesConsumeCommandLikeTokensWithoutChangingContext) {
    EXPECT_EQ(complete({"build", "-p", "self", "--ca"}).words,
              (std::vector<std::string>{"--cache"}));
    EXPECT_EQ(complete({"build", "--cache", "l"}).words,
              (std::vector<std::string>{"local"}));
    EXPECT_EQ(complete({"build", "--cache=l"}).words,
              (std::vector<std::string>{"--cache=local"}));
    EXPECT_TRUE(complete({"build", "--package", ""}).words.empty());
    EXPECT_FALSE(complete({"build", "--package", ""}).files);
}

TEST(ShellCompletion, PathsUseShellNativeCompletionAndPassthroughIsNotMcpp) {
    EXPECT_TRUE(complete({"build", "-o", "some path/"}).files);
    EXPECT_TRUE(complete({"build", "--output=some path/"}).files);
    EXPECT_EQ(complete({"build", "--output=some path/"}).file_prefix, "--output=");
    EXPECT_TRUE(complete({"build", "-osome path/"}).files);
    auto result = complete({"build", "--", "--cache"});
    EXPECT_TRUE(result.words.empty());
    EXPECT_TRUE(result.files);
}

TEST(ShellCompletion, RecordingMetadataPreservesTheOriginalParserAndAction) {
    bool called = false;
    auto app = application();
    (void)app.action([&](const cl::ParsedArgs&) { called = true; });
    std::vector<std::string> argv{"mcpp", "--offline"};
    auto parsed = app.parse_from(argv);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->is_flag_set("offline"));
    app.run(*parsed);
    EXPECT_TRUE(called);
    auto result = complete({"build", "--cache", ""});
    EXPECT_EQ(result.words, (std::vector<std::string>{"global", "local", "off"}));
}
