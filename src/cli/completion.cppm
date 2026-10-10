module;
#include <cstdio>

export module mcpp.cli.completion;

import std;
import mcpplibs.cmdline;
import mcpp.home;
import mcpp.shell;

export namespace mcpp::cli::completion {

struct Command {
    std::string name;
    std::string description;
    std::vector<mcpplibs::cmdline::Option> options;
    std::vector<Command> children;
};

// cmdline 0.0.1 has no introspection API. Record metadata at the same point
// that the parser receives it, rather than maintaining a second command list.
class App : public mcpplibs::cmdline::App {
    using Base = mcpplibs::cmdline::App;
public:
    Command command;
    explicit App(std::string_view name) : Base(name), command{std::string(name), {}, {}, {}} {}
    App& version(std::string_view v) { (void)Base::version(v); return *this; }
    App& description(std::string_view v) {
        command.description = v;
        (void)Base::description(v);
        return *this;
    }
    App& arg(mcpplibs::cmdline::Arg v) { (void)Base::arg(std::move(v)); return *this; }
    App& option(mcpplibs::cmdline::Option v) {
        command.options.push_back(v);
        (void)Base::option(std::move(v));
        return *this;
    }
    App& subcommand(App v) {
        command.children.push_back(std::move(v.command));
        (void)Base::subcommand(std::move(v));
        return *this;
    }
    template<class Fn> App& action(Fn&& fn) {
        (void)Base::action(std::forward<Fn>(fn));
        return *this;
    }
};

struct Result {
    std::vector<std::string> words;
    bool files = false;
    std::string file_prefix;
};

std::vector<std::string> values(const Command& command, const mcpplibs::cmdline::Option& option) {
    const auto& name = option.long_name;
    if (name == "cache") return {"global", "local", "off"};
    if (name == "profile") return {"dev", "release"};
    if (name == "mirror") return {"CN", "GLOBAL"};
    if (name == "message-format") return {"human", "json"};
    if (name == "format") {
        if (command.name == "pack") return {"tar", "dir"};
        return {"json"};
    }
    if (name == "spec" && command.name == "build-database") return {"s1", "compile-commands"};
    return {};
}

bool file_value(const mcpplibs::cmdline::Option& option) {
    return option.value_name_ == "PATH" || option.value_name_ == "FILE"
        || option.value_name_ == "DIR";
}

// words excludes the executable and includes the current (possibly empty)
// word last. No parsing or command action is executed for partial input.
Result candidates(const Command& root, std::span<const std::string> words) {
    if (words.empty()) return {};
    const Command* current = &root;
    std::vector<const mcpplibs::cmdline::Option*> globals;
    for (const auto& option : root.options)
        if (option.global_) globals.push_back(&option);
    auto find_option = [&](std::string_view token) -> const mcpplibs::cmdline::Option* {
        for (const auto& option : current->options)
            if (option.matches(token)) return &option;
        for (const auto* option : globals)
            if (option->matches(token)) return option;
        return nullptr;
    };
    const mcpplibs::cmdline::Option* pending = nullptr;
    bool positional = false;
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        std::string_view word = words[i];
        if (pending) { pending = nullptr; continue; }
        if (word == "--") return {{}, true};
        if (word.starts_with('-')) {
            auto eq = word.find('=');
            const auto* option = find_option(word.substr(0, eq));
            if (!option && word.size() > 2 && !word.starts_with("--"))
                option = find_option(word.substr(0, 2));
            if (option && option->takes_value_ && eq == std::string_view::npos
                && (word.starts_with("--") || word.size() == 2)) pending = option;
            continue;
        }
        auto child = std::ranges::find(current->children, word, &Command::name);
        if (!positional && child != current->children.end()) current = &*child;
        else positional = true;
    }
    std::string_view prefix = words.back();
    std::string attached;
    if (!pending && prefix.starts_with('-')) {
        const auto eq = prefix.find('=');
        if (eq != std::string_view::npos) {
            pending = find_option(prefix.substr(0, eq));
            attached = prefix.substr(0, eq + 1);
            prefix.remove_prefix(eq + 1);
        } else if (prefix.size() > 2 && !prefix.starts_with("--")) {
            const auto* option = find_option(prefix.substr(0, 2));
            if (option && option->takes_value_) {
                pending = option;
                attached = prefix.substr(0, 2);
                prefix.remove_prefix(2);
            }
        }
    }
    Result result;
    auto add = [&](std::string word) {
        if (word.starts_with(prefix)) result.words.push_back(attached + word);
    };
    if (pending) {
        for (auto word : values(*current, *pending)) add(std::move(word));
        result.files = file_value(*pending);
        if (result.files) result.file_prefix = attached;
    } else if (prefix.starts_with('-')) {
        add("--help"); add("-h");
        add("--version"); add("-V");
        auto add_option = [&](const auto& option) {
            if (!option.long_name.empty()) add("--" + option.long_name);
            if (option.short_) add(std::string("-") + option.short_);
        };
        for (const auto& option : current->options) add_option(option);
        for (const auto* option : globals) add_option(*option);
    } else if (current->name == "completion" && !positional) {
        for (const auto* shell : {"bash", "zsh", "pwsh", "fish"}) add(shell);
    } else if (!positional && !current->children.empty()) {
        for (const auto& child : current->children)
            if (!child.description.starts_with("(internal")) add(child.name);
    } else result.files = true;
    std::ranges::sort(result.words);
    auto tail = std::ranges::unique(result.words);
    result.words.erase(tail.begin(), tail.end());
    return result;
}

// The first output line directs the adapter; subsequent lines are literal
// candidates. Shell adapters never evaluate output as shell code.
int query(const Command& root, int argc, char** argv) {
    std::vector<std::string> words;
    for (int i = 2; i < argc; ++i) words.emplace_back(argv[i]);
    // PowerShell before 7.3 drops empty native arguments. Its adapter sends
    // the current word with a marker so an empty prefix survives argv.
    if (!words.empty() && words.back().starts_with("__mcpp_word__"))
        words.back().erase(0, std::string_view("__mcpp_word__").size());
    auto result = candidates(root, words);
    std::println("{}", result.files ? "files" : "words");
    if (result.files && !result.file_prefix.empty()) std::println("{}", result.file_prefix);
    for (const auto& word : result.words) std::println("{}", word);
    return 0;
}

int install_command(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::shell::install(mcpp::home::root(), parsed.positional(0));
}

} // namespace mcpp::cli::completion
