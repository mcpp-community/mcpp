// mcpp.invocation — the environment of this invocation of mcpp.
//
// mcpp states variables for itself and for every program it starts: the home
// it uses (MCPP_HOME), the constraints of the run (MCPP_OFFLINE,
// MCPP_DENY_EFFECTS), a hook's variables, a toolchain's PATH on Windows. It
// does not write them into its own process environment: mcpplibs.os reads that
// environment once, as it was given, and a program it starts receives what
// its caller states (design 2026-10-26 §25.3). This module is the statement:
// an override of the started environment, read by every lookup mcpp makes and
// given to every program mcpp starts.
export module mcpp.invocation;

import std;

export namespace mcpp::invocation {

// A variable's value: this invocation's statement, else the environment mcpp
// was started with. Unset and empty are one answer, nullopt.
std::optional<std::string> get(std::string_view name);

// States a variable for this invocation and every program it starts.
void set(std::string name, std::string value);
// States that a variable is absent, whatever mcpp was started with.
void unset(std::string name);

// Every variable whose name begins with `prefix`, as `NAME=value`, sorted,
// empty values left out: for an identity that changes when any of a family of
// variables does.
std::vector<std::string> with_prefix(std::string_view prefix);

// The statements, as a started program receives them: a value sets the
// variable, nullopt removes it.
std::vector<std::pair<std::string, std::optional<std::string>>> overrides();

// A variable stated for a scope, the previous statement restored after it.
class ScopedEnv {
public:
    ScopedEnv(std::string name, std::optional<std::string> value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    std::string name_;
    std::optional<std::optional<std::string>> previous_;   // the statement before, if any
};

// A directory named by a variable; nothing when it is unset or empty.
std::optional<std::filesystem::path> env_directory(std::string_view name);

// `dir` before PATH, for this invocation's lookups and its programs.
void prepend_path(const std::filesystem::path& dir);

// A program found on this invocation's PATH (with PATHEXT on Windows).
std::optional<std::filesystem::path> which(std::string_view program);

// The file this program was started from.
std::filesystem::path self_exe_path();

// The identifier the system gives this process.
long process_id();

}  // namespace mcpp::invocation
